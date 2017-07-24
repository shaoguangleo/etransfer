// Implementation file
#include <utilities.h>
#include <etdc_etdserver.h>

// C++ headerts
//#include <regex>
#include <mutex>
#include <memory>
#include <thread>
#include <functional>

// Plain-old-C
#include <glob.h>
#include <string.h>

namespace etdc {

    // Two specializations of reading off_t from std::string
    template <>
    void string2off_t<long int>(std::string const& s, long int& o) {
        o = std::stol(s);
    }
    template <>
    void string2off_t<long long int>(std::string const& s, long long int& o) {
        o = std::stoll(s);
    }

    // parse "<proto/host:port>" into sockname_type
    sockname_type decode_data_addr(std::string const& s) {
        static const std::string    ipv6_lit{ "[:0-9a-zA-Z]+(/[0-9]{1,3})?(%[a-zA-Z0-9]+)?" };
        //                                                  3             4
        // From: https://stackoverflow.com/a/3824105 
        // Also - need to check that host name length <= 255
        static const std::string    valid_host( "(([a-zA-Z0-9]|[a-zA-Z0-9][a-zA-Z0-9\\-]{0,61}[a-zA-Z0-9])"
        //                                       56
                                                "(\\.([a-zA-Z0-9]|[a-zA-Z0-9][a-zA-Z0-9\\-]{0,61}[a-zA-Z0-9]))*)");
        //                                       7   8                
        static const std::regex     rxSockName("^<([^/]+)/(\\["+ipv6_lit+"\\]|" + valid_host + "):([0-9]+)>$");
                                            //    1       2                                       9 
                                            //    proto   host                                    port

        std::smatch fields;
        ETDCASSERT(std::regex_match(s, fields, rxSockName), "The string '" << s << "' is not a valid data address designator");
        ETDCASSERT(fields[5].length()<=255, "Host names can not be longer than 255 characters (RFC1123)");
        ETDCDEBUG(4, "decode_data_addr: 1='" << fields[1].str() << "' 2='" << fields[2].str() << "' 9='" << fields[9].str() << "'" << std::endl);

        // OK now extract the fields!
        return mk_sockname(fields[1].str(), unbracket(fields[2].str()), port(fields[9].str()));
    }


    /////////////////////////////////////////////////////////////////////////////////////////
    //
    //     This is the real ETDServer.
    //     An instance of this may be running in the daemon but also inside
    //     the client, if either end of the transfer is a local path :-)
    //
    /////////////////////////////////////////////////////////////////////////////////////////
   
    filelist_type ETDServer::listPath(std::string const& path, bool allow_tilde) const {
        ETDCASSERT(!path.empty(), "We do not allow listing an empty path");

        // glob() is MT unsafe so we had better make sure only one thread executes this
        //static std::mutex       globMutex;
        std::string                                gPath( path );

        // If the path ends with "/" we add "*" because the client wishes to
        // list the contents of the directory
        if( *path.rbegin()=='/' )
            gPath.append("*");

        // Grab lock + execute 
        // Allocate zero-initialized struct and couple with correct deleter for when it goes out of scope
        int                                        globFlags{ GLOB_MARK };
        std::unique_ptr<glob_t, void(*)(glob_t*)>  files{new etdc::Zero<glob_t>(),
                                                         [](glob_t* p) { ::globfree(p); delete p;} };

#ifdef GLOB_TILDE
        globFlags |= (allow_tilde ? GLOB_TILDE : 0);
#else
        // No tilde support, so if we find '~' in path and ~ expansion requested
        // then we can return a useful error message
        if( allow_tilde && gPath.find('~')!=std::string::npos )
            throw std::domain_error("The target O/S does not support the requested tilde expansion");
#endif
        // Make the glob go fast(er) [NOSORT] - we'll do that ourselves when we have all the paths in memory
        //::glob(path.c_str(), GLOB_MARK|/*GLOB_NOSORT|*/, nullptr, files.get());
        ::glob(gPath.c_str(), globFlags, nullptr, files.get());
        //
        //    std::lock_guard<std::mutex> scopedlock(globMutex);
        //}
        
        // Now that we have the results, we can 
        return filelist_type(&files->gl_pathv[0], &files->gl_pathv[files->gl_pathc]);
    }

    //////////////////////////////////////////////////////////////////////////////////////
    //
    // Attempt to set up resources for writing to a file
    // return our UUID that the client must use to write to the file
    //
    //////////////////////////////////////////////////////////////////////////////////////
    result_type ETDServer::requestFileWrite(std::string const& path, openmode_type mode) {
        static const std::set<openmode_type> allowedModes{openmode_type::New, openmode_type::OverWrite, openmode_type::Resume, openmode_type::SkipExisting};

        // We must check-and-insert-if-ok into shared state.
        // This has to be atomic, so we'll grab the lock
        // until we're completely done.
        auto&                       shared_state( __m_shared_state.get() );
        std::lock_guard<std::mutex> lk( shared_state.lock );
        auto&                       transfers( shared_state.transfers );

        // Before we allow doing anything at all we must make sure
        // that we're not already busy doing something else
        ETDCASSERT(transfers.find(__m_uuid)==transfers.end(), "requestFileWrite: this server is already busy");

        const std::string nPath( detail::normalize_path(path) );

        // Attempt to open path new, write or append [reject read!]
        ETDCASSERT(allowedModes.find(mode)!=std::end(allowedModes),
                   "invalid open mode for requestFileWrite(" << path << ")");

        // Before doing anything - see if this server already has an entry for this (normalized) path -
        // we cannot honour multiple write attempts (not even if it was already open for reading!)
        const auto  pathPresent = (std::find_if(std::begin(transfers), std::end(transfers),
                                                [&](transfermap_type::value_type const& vt) { return vt.second->path==nPath; })
                                   != std::end(transfers));
        ETDCASSERT(pathPresent==false, "requestFileWrite(" << path << ") - the path is already in use");

        // Transform to int argument to open(2) + append some flag(s) if necessary/available
        int  omode = static_cast<int>(mode);

        // Insider trick ... SkipExisting is bitwise complement of the real open flags
        if( mode==openmode_type::SkipExisting )
            omode = ~omode;

#if O_LARGEFILE
        // set large file if the current system has it
        omode |= O_LARGEFILE;
#endif

        // Note: etdc_file(...) c'tor will create the whole directory tree if necessary.
        //       Because it may/may not have to create, we add the file permission bits
        etdc_fdptr      fd( new etdc_file(nPath, omode, 0644) );
        const off_t     fsize{ fd->lseek(fd->__m_fd, 0, SEEK_END) };
        //const uuid_type uuid{ uuid_type::mk() };

        ETDCASSERT(transfers.emplace(__m_uuid, std::unique_ptr<transferprops_type>(new etdc::transferprops_type(fd, nPath, mode))).second,
                   "Failed to insert new entry, request file write '" << path << "'");
        // and return the uuid + alreadyhave
        return result_type(__m_uuid, fsize);
    }

    result_type ETDServer::requestFileRead(std::string const& path, off_t alreadyhave) {
        // We must check-and-insert-if-ok into shared state.
        // This has to be atomic, so we'll grab the lock
        // until we're completely done.
        auto&                       shared_state( __m_shared_state.get() );
        std::lock_guard<std::mutex> lk( shared_state.lock );
        auto&                       transfers( shared_state.transfers );

        // Check if we're not already busy
        ETDCASSERT(transfers.find(__m_uuid)==transfers.end(), "requestFileRead: this server is already busy");

        // Before doing anything - see if this server already has an entry for this (normalized) path -
        // we can only honour this request if it's opened for reading [multiple readers = ok]
        const std::string nPath( detail::normalize_path(path) );
        const auto  pathPtr = std::find_if(std::begin(transfers), std::end(transfers),
                                           std::bind([&](std::string const& p) { return p==nPath; },
                                                     std::bind(std::mem_fn(&transferprops_type::path), std::bind(etdc::snd_type(), std::placeholders::_1))));
        ETDCASSERT(pathPtr==std::end(transfers) || pathPtr->second->openMode==openmode_type::Read,
                   "requestFileRead(" << path << ") - the path is already in use");

        // Transform to int argument to open(2) + append some flag(s) if necessary/available
        int  omode = static_cast<int>(etdc::openmode_type::Read);

#if O_LARGEFILE
        // set large file if the current system has it
        omode |= O_LARGEFILE;
#endif

        // Note: etdc_file(...) c'tor will create the whole directory tree if necessary.
        // Because openmode is read, then we don't have to pass the file permissions; either it's there or it isn't
        etdc_fdptr      fd( new etdc_file(nPath, omode) );
        const off_t     sz{ fd->lseek(fd->__m_fd, 0, SEEK_END) };
        //const uuid_type uuid{ uuid_type::mk() };

        // Assert that we can seek to the requested position
        ETDCASSERT(fd->lseek(fd->__m_fd, alreadyhave, SEEK_SET)!=static_cast<off_t>(-1),
                   "Cannot seek to position " << alreadyhave << " in file " << path << " - " << etdc::strerror(errno));

        auto insres = transfers.emplace(__m_uuid, std::unique_ptr<transferprops_type>( new etdc::transferprops_type(fd, nPath, openmode_type::Read)));
        ETDCASSERT(insres.second, "Failed to insert new entry, request file read '" << path << "'");
        return result_type(__m_uuid, sz-alreadyhave);
    }

    dataaddrlist_type ETDServer::dataChannelAddr( void ) const {
        auto&                       shared_state( __m_shared_state.get() );
        std::lock_guard<std::mutex> lk( shared_state.lock );
        return shared_state.dataaddrs;
    }

    bool ETDServer::removeUUID(etdc::uuid_type const& uuid) {
        ETDCASSERT(uuid==__m_uuid, "Cannot remove someone else's UUID!");

        // We need to do some thinking about locking sequence because we need
        // a lock on the shared state *and* a lock on the transfer
        // before we can attempt to remove it.
        // To prevent deadlock we may have to relinquish the locks and start again.
        // What that means is that if we fail to lock both atomically, we must start over:
        //  lock shared state and (attempt to) find the transfer
        // because after we've released the shared state lock, someone else may have snuck in
        // and deleted or done something bad with the transfer i.e. we cannot do a ".find(uuid)" once 
        // and assume the iterator will remain valid after releasing the lock on shared_state
        etdc::etd_state&                    shared_state( __m_shared_state.get() );
        std::unique_ptr<transferprops_type> removed;
        while( true ) {
            // 1. lock shared state
            std::unique_lock<std::mutex>     lk( shared_state.lock );
            // 2. find if there is an entry in the map for us
            etdc::transfermap_type::iterator ptr = shared_state.transfers.find(__m_uuid);
            
            // No? OK then we're done
            if( ptr==shared_state.transfers.end() )
                return false;

            // Now we must do try_lock on the transfer - if that fails we sleep and start from the beginning
            //std::unique_lock<std::mutex>     sh( *ptr->second.lockPtr, std::try_to_lock );
            std::unique_lock<std::mutex>     sh( ptr->second->lock, std::try_to_lock );
            if( !sh.owns_lock() ) {
                // we must release the lock on shared state before sleeping
                // for a bit or else no-one can change anything [because we
                // hold the lock to shared state ...]
                lk.unlock();
                // *now* we sleep for a bit and then try again
                std::this_thread::sleep_for( std::chrono::microseconds(42) );
                //std::this_thread::sleep_for( std::chrono::seconds(1) );
                continue;
            }
            // Right, we now hold both locks!
            transferprops_type&  transfer( *ptr->second );
            transfer.fd->close(transfer.fd->__m_fd);
            // We cannot erase the transfer immediately: we hold the lock that is contained in it
            // so what we do is transfer the lock out of the transfer and /then/ erase the entry.
            // And when we finally return, then the lock will be unlocked and the unique pointer
            // deleted
            //transfer_lock = std::move(transfer.lockPtr);
            // move the data out of the transfermap
            //std::swap(removed, ptr->second);
            removed.swap( ptr->second );
            // OK lock is now moved out of the transfer, so now it's safe to erase the entry
            // OK the uniqueptr to the transfer is now moved out of the transfermap, so now it's safe to erase the entry
            shared_state.transfers.erase( ptr );
            break;
        }
        return true;
    }

    bool ETDServer::sendFile(uuid_type const& srcUUID, uuid_type const& dstUUID, 
                             off_t todo, dataaddrlist_type const& dataAddrs) {
        // 1a. Verify that the srcUUID is our UUID
        ETDCASSERT(srcUUID==__m_uuid, "The srcUUID '" << srcUUID << "' is not our UUID");

        // We need to protect our transfer so we need to do deadlock avoidance
        // with re-searching our UUID until we have both locks
        etdc::etd_state&                 shared_state( __m_shared_state.get() );

        // Make it loop until all bytes are transferred
        while( todo>0 ) {
            // 2a. lock shared state
            std::unique_lock<std::mutex>     lk( shared_state.lock );
            // 2b. assert that there is an entry for us, indicating that we ARE configured
            etdc::transfermap_type::iterator ptr = shared_state.transfers.find(__m_uuid);

            ETDCASSERT(ptr!=shared_state.transfers.end(), "This server was not initialized yet");
            
            // Now we must do try_lock on the transfer - if that fails we sleep and start from the beginning
            //std::unique_lock<std::mutex>     sh( *ptr->second.lockPtr, std::try_to_lock );
            std::unique_lock<std::mutex>     sh( ptr->second->lock, std::try_to_lock );
            if( !sh ) {
                // we must manually unlock the shared state before sleeping
                // or else no-one will be able to change anything
                lk.unlock();
                // *now* sleep for a bit and then try again ...
                std::this_thread::sleep_for( std::chrono::microseconds(19) );
                continue;
            }
            // Right, we now hold both locks!
            // At this point we don't need the shared_state lock anymore - we've found our entry and we've locked it
            // So no-one can remove the entry from under us until we're done
            lk.unlock();

            // Verify that indeed we are configured for file read
            transferprops_type&  transfer( *ptr->second );

            ETDCASSERT(transfer.openMode==openmode_type::Read, "This server was initialized, but not for reading a file");

            // Great. Now we attempt to connect to the remote end
            etdc::etdc_fdptr    dstFD;
            std::ostringstream  tried;

            for(auto addr: dataAddrs) {
                try {
                    dstFD = mk_client(get_protocol(addr), get_host(addr), get_port(addr));
                    ETDCDEBUG(2, "sendFile/connected to " << addr << std::endl);
                    break;
                }
                catch( std::exception const& e ) {
                    tried << addr << ": " << e.what() << ", ";
                }
                catch( ... ) {
                    tried << addr << ": unknown exception" << ", ";
                }
            }
            ETDCASSERT(dstFD, "Failed to connect to any of the data servers: " << tried.str());

            // Weehee! we're connected!
            const size_t                     bufSz( 10*1024*1024 );
            std::unique_ptr<unsigned char[]> buffer(new unsigned char[bufSz]);

            // Create message header
            std::ostringstream  msg_buf;
            msg_buf << "{ uuid:" << dstUUID << ", sz:" << todo << "}";

            const std::string   msg( msg_buf.str() );
            dstFD->write(dstFD->__m_fd, msg.data(), msg.size());

            while( todo>0 ) {
                const size_t  n = std::min((size_t)todo, bufSz);
                ETDCASSERTX(transfer.fd->read(transfer.fd->__m_fd, &buffer[0], n)==(ssize_t)n);
                ETDCASSERTX(dstFD->write(dstFD->__m_fd, &buffer[0], n)==(ssize_t)n);
                todo -= (off_t)n;
            }
            // if we make it out of the loop, todo should be <= 0 and terminate the outer loop
            // wait here until the recipient has acknowledged receipt of all bytes
            char    ack;
            ETDCDEBUG(4, "sendFile: waiting for remote ACK ..." << std::endl);
            dstFD->read(dstFD->__m_fd, &ack, 1);
            ETDCDEBUG(4, "sendFile: ... got it" << std::endl);
        }
        ETDCDEBUG(4, "sendFile: done!" << std::endl);
        return true;
    }

    bool ETDServer::getFile(uuid_type const& srcUUID, uuid_type const& dstUUID, 
                            off_t todo, dataaddrlist_type const& dataAddrs) {
        // 1a. Verify that the dstUUID is our UUID
        ETDCASSERT(dstUUID==__m_uuid, "The dstUUID '" << dstUUID << "' is not our UUID");

        // We need to protect our transfer so we need to do deadlock avoidance
        // with re-searching our UUID until we have both locks
        etdc::etd_state&                 shared_state( __m_shared_state.get() );

        // Make it loop until all bytes are transferred
        while( todo>0 ) {
            // 2a. lock shared state
            std::unique_lock<std::mutex>     lk( shared_state.lock );
            // 2b. assert that there is an entry for us, indicating that we ARE configured
            etdc::transfermap_type::iterator ptr = shared_state.transfers.find(__m_uuid);

            ETDCASSERT(ptr!=shared_state.transfers.end(), "This server was not initialized yet");
            
            // Now we must do try_lock on the transfer - if that fails we sleep and start from the beginning
            //std::unique_lock<std::mutex>     sh( *ptr->second.lockPtr, std::try_to_lock );
            std::unique_lock<std::mutex>     sh( ptr->second->lock, std::try_to_lock );
            if( !sh ) {
                // Manually unlock the shared state or else nobody won't be
                // able to change anything!
                lk.unlock();

                // *now* we sleep for a bit and then try again
                std::this_thread::sleep_for( std::chrono::microseconds(23) );
                continue;
            }
            // Right, we now hold both locks!
            // At this point we don't need the shared_state lock anymore - we've found our entry and we've locked it
            // So no-one can remove the entry from under us until we're done
            lk.unlock();

            // Verify that indeed we are configured for file write
            // Note that we do NOT include 'skip existing' in here - the
            // point is that we don't want to write to such a file!
            transferprops_type&  transfer( *ptr->second );
            static const std::set<etdc::openmode_type>  allowedWriteModes{ openmode_type::OverWrite, openmode_type::New, openmode_type::Resume };

            ETDCASSERT(allowedWriteModes.find(transfer.openMode)!=allowedWriteModes.end(),
                       "This server was initialized, but not for writing to file");

            // Great. Now we attempt to connect to the remote end
            etdc::etdc_fdptr    dstFD;
            std::ostringstream  tried;

            for(auto addr: dataAddrs) {
                try {
                    dstFD = mk_client(get_protocol(addr), get_host(addr), get_port(addr));
                    ETDCDEBUG(2, "getFile/connected to " << addr << std::endl);
                    break;
                }
                catch( std::exception const& e ) {
                    tried << addr << ": " << e.what() << ", ";
                }
                catch( ... ) {
                    tried << addr << ": unknown exception" << ", ";
                }
            }
            ETDCASSERT(dstFD, "Failed to connect to any of the data servers: " << tried.str());

            // Weehee! we're connected!
            const size_t                     bufSz( 10*1024*1024 );
            std::unique_ptr<unsigned char[]> buffer(new unsigned char[bufSz]);

            // Create message header
            std::ostringstream  msg_buf;
            msg_buf << "{ uuid:" << srcUUID << ", push:1, sz:" << todo << "}";

            const std::string   msg( msg_buf.str() );
            dstFD->write(dstFD->__m_fd, msg.data(), msg.size());

            while( todo>0 ) {
                // Read at most bufSz bytes
                const ssize_t n = dstFD->read(dstFD->__m_fd, &buffer[0], bufSz);
                if( n>0 ) {
                    ETDCASSERTX(transfer.fd->write(transfer.fd->__m_fd, &buffer[0], n)==n);
                    todo -= (off_t)n;
                }
            }
            // if we make it out of the loop, todo should be <= 0 and terminate the outer loop
            // Send ACK 
            const char ack{ 'y' };
            ETDCDEBUG(4, "ETDServer::getFile/got all bytes, sending ACK ..." << std::endl);
            dstFD->write(dstFD->__m_fd, &ack, 1);
            ETDCDEBUG(4, "ETDServer::getFile/... done." << std::endl);
        }
        return true;
    }

    ETDServer::~ETDServer() {
        // we must clean up our UUID!
        try {
            this->removeUUID( __m_uuid );
        }
        catch(...) {}
    }


    /////////////////////////////////////////////////////////////////////////////////////////
    //
    //     This is an ETDProxy.
    //     It /looks/ like a real ETDServer but behind the scenes it communicates
    //     with a remote instance of ETDServer, w/o the client knowing anything 
    //     about that
    //
    /////////////////////////////////////////////////////////////////////////////////////////
    //static std::basic_regex<unsigned char> rxLine;
    static const std::regex::flag_type etdc_rxFlags = (std::regex::ECMAScript | std::regex::icase);
    static const std::regex            rxLine("([^\\r\\n]+)[\\r\\n]+");
    static const std::regex            rxReply("^(OK|ERR)(\\s+(\\S.*)?)?$", etdc_rxFlags);
                                             //  1       2    3   submatch numbers

    template <typename InputIter, typename OutputIter,
              typename RegexIter  = std::regex_iterator<InputIter>,
              typename RetvalType = typename std::match_results<InputIter>::size_type>
    static RetvalType getReplies(InputIter f, InputIter l, OutputIter o) {
        static const RegexIter  rxIterEnd{};

        RetvalType endpos = 0;
        for(RegexIter line = RegexIter(f, l, rxLine); line!=rxIterEnd; line++) {
            // Found another line: append just the non-newline stuff to output and update endpos
            *o++   = (*line)[1].str();
            endpos = line->position() + line->length();
        }
        return endpos;
    }

    filelist_type ETDProxy::listPath(std::string const& path, bool) const {
        std::ostringstream   msgBuf;

        msgBuf << "list " << path << '\n';
        const std::string  msg( msgBuf.str() );

        ETDCDEBUG(4, "ETDProxy::listPath/sending message '" << msg << "'" << std::endl);
        ETDCASSERTX(__m_connection->write(__m_connection->__m_fd, msg.data(), msg.size())==(ssize_t)msg.size());

        // And await the reply
        const size_t            bufSz( 16384 );
        std::unique_ptr<char[]> buffer(new char[bufSz]);

        bool          finished{ false };
        size_t        curPos{ 0 };
        std::string   state;
        filelist_type rv;

        while( !finished && curPos<bufSz ) {
            const ssize_t n = __m_connection->read(__m_connection->__m_fd, &buffer[curPos], bufSz-curPos);

            // did we read anything?
            ETDCASSERT(n>0, "Failed to read data from remote end");
            curPos += n;

            // Parse the reply so far
            std::list<std::string> lines;
            std::smatch::size_type endpos = getReplies(&buffer[0], &buffer[curPos], std::back_inserter(lines));
            auto                   line = lines.begin();

            // Check what we got back
            for(; !finished && line!=lines.end(); line++) {
                std::smatch   fields;

                ETDCDEBUG(4, "listPath/reply from server: '" << *line << "'" << std::endl);
                ETDCASSERT(std::regex_match(*line, fields, rxReply), "Server replied with an invalid line");
                // error code must be either == current state (all lines starting with OK)
                // or state.empty && error code = ERR; we cannot have OK, OK, OK, ERR -> it's either ERR or OK, OK, OK, ... OK
                ETDCASSERT(state.empty() || (state=="OK" && fields[1].str()==state),
                           "The server changed its mind about the success of the call in the middle of the reply");
                state  = fields[1].str();

                const std::string   info( fields[3].str() ); 

                // Translate error into an exception
                if( state=="ERR" )
                    throw std::runtime_error(std::string("listPath(")+path+") failed - " + (info.empty() ? "<unknown reason>" : info));

                // This is the end-of-reply sentinel: a single OK by itself
                if( (finished=(state=="OK" && info.empty()))==true )
                    continue;
                // Otherwise append the entry to the list of paths
                rv.push_back( info );
            }
            ETDCASSERT(line==lines.end(), "There are unprocessed lines of reply from the server. This is probably a protocol error.");
            // Processed all lines in the reply so far.
            // So we move all processed bytes to begin of buffer
            ::memmove(&buffer[0], &buffer[endpos], curPos - endpos);
            curPos -= endpos;
        }
        ETDCASSERT(curPos==0, "listPath: there are " << curPos << " unconsumed bytes left in the input. This is likely a protocol error.");
        return rv;
    }

    result_type ETDProxy::requestFileWrite(std::string const& file, openmode_type om) {
        static const std::regex  rxUUID( "^UUID:(\\S+)$", etdc_rxFlags);
        static const std::regex  rxAlreadyHave( "^AlreadyHave:([0-9]+)$", etdc_rxFlags);
        std::ostringstream       msgBuf;

        msgBuf << "write-file-" << om << " " << file << '\n';
        const std::string  msg( msgBuf.str() );

        ETDCDEBUG(4, "ETDProxy::requestFileWrite/sending message '" << msg << "' sz=" << msg.size() << std::endl);
        ETDCASSERTX(__m_connection->write(__m_connection->__m_fd, msg.data(), msg.size())==(ssize_t)msg.size());

        // And await the reply
        const size_t               bufSz( 2048 );
        std::unique_ptr<char[]>    buffer(new char[bufSz]);

        bool                       finished{ false };
        size_t                     curPos{ 0 };
        std::string                status_s, info;
        std::unique_ptr<off_t>     filePos{};
        std::unique_ptr<uuid_type> curUUID{};

        while( !finished && curPos<bufSz ) {
            const ssize_t n = __m_connection->read(__m_connection->__m_fd, &buffer[curPos], bufSz-curPos);

            // did we read anything?
            ETDCASSERT(n>0, "Failed to read data from remote end");
            curPos += n;

            std::vector<std::string>  lines;
            auto                      endpos = getReplies(&buffer[0], &buffer[curPos], std::back_inserter(lines));
            auto                      line = lines.begin();

            // Check what we got back
            for(; !finished && line!=lines.end(); line++) {
                std::smatch   fields;

                if( std::regex_match(*line, fields, rxUUID) ) {
                    ETDCASSERT(!curUUID, "Server had already sent a UUID");
                    curUUID = std::move( std::unique_ptr<uuid_type>(new uuid_type(fields[1].str())) );
                } else if( std::regex_match(*line, fields, rxAlreadyHave) ) {
                    ETDCASSERT(!filePos, "Server had already sent file position");
                    filePos = std::move( std::unique_ptr<off_t>(new off_t) );
                    string2off_t(fields[1].str(), *filePos);
                } else if( std::regex_match(*line, fields, rxReply) ) {
                    // We get OK (optional stuff)
                    // or     ERR (optional error message)
                    // Either will mean end-of-parsing
                    status_s = fields[1].str();
                    info     = fields[3].str();
                    finished = true;
                } else {
                    ETDCASSERT(false, "requestFileWrite: the server sent a reply that we did not recognize: " << *line);
                }
            }
            ETDCASSERT(line==lines.end(), "requestFileWrite: there are unprocessed lines of input left, this means the server sent an erroneous reply.");
            // Now we're sure we've processed all lines in the reply so far.
            // So we move all processed bytes to begin of buffer
            ::memmove(&buffer[0], &buffer[endpos], curPos - endpos);
            curPos -= endpos;
        }
        // We must have consumed all output from the server
        ETDCASSERT(curPos==0, "requestFileWrite: there are " << curPos << " unconsumed server bytes left in the input. This is likely a protocol error.");
        // We must have seen a success reply
        ETDCASSERT(status_s=="OK", "requestFileWrite(" << file << ") failed - " << (info.empty() ? "<unknown reason>" : info));
        // And we must have received both a UUID as well as an AlreadyHave
        ETDCASSERT(filePos && curUUID, "requestFileWrite: the server did NOT send all required fields");
        return result_type{*curUUID, *filePos};
    }

    result_type ETDProxy::requestFileRead(std::string const& file, off_t already_have) {
        static const std::regex  rxUUID( "^UUID:(\\S+)$", etdc_rxFlags);
        static const std::regex  rxRemain( "^Remain:(-?[0-9]+)$", etdc_rxFlags);
        std::ostringstream       msgBuf;

        msgBuf << "read-file " << already_have << " " << file << '\n';
        const std::string  msg( msgBuf.str() );

        ETDCDEBUG(4, "ETDProxy::requestFileRead/sending message '" << msg << "'" << std::endl);
        ETDCASSERTX(__m_connection->write(__m_connection->__m_fd, msg.data(), msg.size())==(ssize_t)msg.size());

        // And await the reply
        const size_t               bufSz( 2048 );
        std::unique_ptr<char[]>    buffer(new char[bufSz]);

        bool                       finished{ false };
        size_t                     curPos{ 0 };
        std::string                info, status_s;
        std::unique_ptr<off_t>     remain{};
        std::unique_ptr<uuid_type> curUUID{};

        while( !finished && curPos<bufSz ) {
            const ssize_t n = __m_connection->read(__m_connection->__m_fd, &buffer[curPos], bufSz-curPos);

            // did we read anything?
            ETDCASSERT(n>0, "Failed to read data from remote end");
            curPos += n;

            std::vector<std::string>  lines;
            auto                      endpos = getReplies(&buffer[0], &buffer[curPos], std::back_inserter(lines));
            auto                      line = lines.begin();

            // Check what we got back
            for(; !finished && line!=lines.end(); line++) {
                std::smatch   fields;

                if( std::regex_match(*line, fields, rxUUID) ) {
                    ETDCASSERT(!curUUID, "Server already sent a UUID");
                    curUUID = std::move( std::unique_ptr<uuid_type>(new uuid_type(fields[1].str())) );
                } else if( std::regex_match(*line, fields, rxRemain) ) {
                    ETDCASSERT(!remain, "Server already sent a file position");
                    remain = std::move( std::unique_ptr<off_t>(new off_t) );
                    string2off_t(fields[1].str(), *remain);
                } else if( std::regex_match(*line, fields, rxReply) ) {
                    // We get OK (optional stuff)
                    // or     ERR (optional error message)
                    // Either will mean end-of-parsing
                    status_s = fields[1].str();
                    info     = fields[3].str(); 
                    finished = true;
                } else {
                    ETDCASSERT(false, "requestFileRead: the server sent a reply that we did not recognize: " << *line);
                }
            }
            ETDCASSERT(line==lines.end(), "requestFileRead: there are unprocessed lines of input left, this means the server sent an erroneous reply.");
            // Now we're sure we've processed all lines in the reply so far.
            // So we move all processed bytes to begin of buffer
            ::memmove(&buffer[0], &buffer[endpos], curPos - endpos);
            curPos -= endpos;
        }
        // We must have consumed all output from the server
        ETDCASSERT(curPos==0, "requestFileRead: there are " << curPos << " unconsumed server bytes left in the input. This is likely a protocol error.");
        // We must have seen a success reply
        ETDCASSERT(status_s=="OK", "requestFileRead(" << file << ") failed - " << (info.empty() ? "<unknown reason>" : info));
        // And we must have received both a UUID as well as an AlreadyHave
        ETDCASSERT(remain && curUUID, "requestFileRead: the server did NOT send all required fields");
        return result_type{*curUUID, *remain};
    }

    dataaddrlist_type ETDProxy::dataChannelAddr( void ) const {
        static const std::string msg{ "data-channel-addr\n" };
        ETDCDEBUG(4, "ETDProxy::dataChannelAddr/sending message '" << msg << "'" << std::endl);
        ETDCASSERTX(__m_connection->write(__m_connection->__m_fd, msg.data(), msg.size())==(ssize_t)msg.size());

        // And await the reply. We don't expect /a lot/ of data channel addrs so don't need a really big buf
        const size_t            bufSz( 2048 );
        std::unique_ptr<char[]> buffer(new char[bufSz]);

        bool              finished{ false };
        size_t            curPos{ 0 };
        std::string       state;
        dataaddrlist_type rv;

        while( !finished && curPos<bufSz ) {
            const ssize_t n = __m_connection->read(__m_connection->__m_fd, &buffer[curPos], bufSz-curPos);

            // did we read anything?
            ETDCASSERT(n>0, "Failed to read data from remote end");
            curPos += n;

            // Parse the reply so far
            std::list<std::string> lines;
            std::smatch::size_type endpos = getReplies(&buffer[0], &buffer[curPos], std::back_inserter(lines));
            auto                   line = lines.begin();

            // Check what we got back
            for(; !finished && line!=lines.end(); line++) {
                std::smatch   fields;

                ETDCDEBUG(4, "dataChannelAddr/reply from server: '" << *line << "'" << std::endl);
                ETDCASSERT(std::regex_match(*line, fields, rxReply), "Server replied with an invalid line");
                // error code must be either == current state (all lines starting with OK)
                // or state.empty && error code = ERR; we cannot have OK, OK, OK, ERR -> it's either ERR or OK, OK, OK, ... OK
                ETDCASSERT(state.empty() || (state=="OK" && fields[1].str()==state),
                           "The server changed its mind about the success of the call in the middle of the reply");
                state  = fields[1].str();

                const std::string   info( fields[3].str() ); 

                // Translate error into an exception
                if( state=="ERR" )
                    throw std::runtime_error(std::string("dataChannelAddr() failed - ") + (info.empty() ? "<unknown reason>" : info));

                // This is the end-of-reply sentinel: a single OK by itself
                // despite it looks like we continue, the while loop will terminate because finish now is .TRUE.
                if( (finished=(state=="OK" && info.empty()))==true )
                    continue;
                // Otherwise append the entry to the list of paths
                rv.push_back( decode_data_addr(info) );
            }
            ETDCASSERT(line==lines.end(), "There are unprocessed lines of reply from the server. This is probably a protocol error.");
            // Processed all lines in the reply so far.
            // So we move all processed bytes to begin of buffer
            ::memmove(&buffer[0], &buffer[endpos], curPos - endpos);
            curPos -= endpos;
        }
        ETDCASSERT(curPos==0, "dataChannelAddr: there are " << curPos << " unconsumed bytes left in the input. This is likely a protocol error.");
        return rv;
    }

    bool ETDProxy::removeUUID(uuid_type const& uuid) {
        std::ostringstream       msgBuf;

        msgBuf << "remove-uuid " << uuid << '\n';
        const std::string  msg( msgBuf.str() );

        ETDCDEBUG(4, "ETDProxy::removeUUID/sending message '" << msg << "'" << std::endl);
        ETDCASSERTX(__m_connection->write(__m_connection->__m_fd, msg.data(), msg.size())==(ssize_t)msg.size());

        // And await the reply. We only allow "OK" or "ERR <msg>"
        // if we allow ~1kB for the <msg> that's quite generous I'd say
        size_t                     curPos{ 0 };
        const size_t               bufSz( 2048 );
        std::unique_ptr<char[]>    buffer(new char[bufSz]);

        while( curPos<bufSz ) {
            const ssize_t n = __m_connection->read(__m_connection->__m_fd, &buffer[curPos], bufSz-curPos);

            // did we read anything?
            ETDCASSERT(n>0, "Failed to read data from remote end");
            curPos += n;

            std::vector<std::string>  lines;
            std::smatch               fields;

            // Discard the return value from getReplies - we don't need to remember where we end in the buffer
            (void)getReplies(&buffer[0], &buffer[curPos], std::back_inserter(lines));

            // If no line(s) yet, read more bytes
            if( lines.empty() )
                continue;

            // If we get >1 line, the client's messin' wiv de heads - we only allow 1 (one) line of reply
            ETDCASSERT(lines.size()==1, "The client sent wrong number of responses - this is likely a protocol error");
            // And that line should match our expectations
            ETDCASSERT(std::regex_match(*lines.begin(), fields, rxReply), "The client sent a non-conforming response");
            // Translate "ERR <Reason>" into an exception
            ETDCASSERT(fields[1].str()=="OK", "removeUUID failed: " << fields[2].str());
            // Otherwise we're done
            break;
        }
        return true;
    }

    bool ETDProxy::sendFile(uuid_type const& srcUUID, uuid_type const& dstUUID, off_t todo, dataaddrlist_type const& dataaddrs) {
        std::ostringstream       msgBuf;

        msgBuf << "send-file " << srcUUID << " " << dstUUID << " " << todo << " ";
        for(auto p = dataaddrs.begin(); p!=dataaddrs.end(); p++)
            msgBuf << ((p!=dataaddrs.begin()) ? "," : "") << *p;
        msgBuf << '\n';
        const std::string  msg( msgBuf.str() );

        ETDCDEBUG(4, "ETDProxy::sendFile/sending message '" << msg << "'" << std::endl);
        ETDCASSERTX(__m_connection->write(__m_connection->__m_fd, msg.data(), msg.size())==(ssize_t)msg.size());

        // And await the reply. We only allow "OK" or "ERR <msg>"
        // if we allow ~1kB for the <msg> that's quite generous I'd say
        size_t                     curPos{ 0 };
        const size_t               bufSz( 2048 );
        std::unique_ptr<char[]>    buffer(new char[bufSz]);

        while( curPos<bufSz ) {
            const ssize_t n = __m_connection->read(__m_connection->__m_fd, &buffer[curPos], bufSz-curPos);

            // did we read anything?
            ETDCASSERT(n>0, "Failed to read data from remote end");
            curPos += n;

            std::vector<std::string>  lines;
            std::smatch               fields;

            // Discard the return value from getReplies - we don't need to remember where we end in the buffer
            (void)getReplies(&buffer[0], &buffer[curPos], std::back_inserter(lines));

            // If no line(s) yet, read more bytes
            if( lines.empty() )
                continue;

            // If we get >1 line, the client's messin' wiv de heads - we only allow 1 (one) line of reply
            ETDCASSERT(lines.size()==1, "The client sent wrong number of responses - this is likely a protocol error");
            // And that line should match our expectations
            ETDCASSERT(std::regex_match(*lines.begin(), fields, rxReply), "The client sent a non-conforming response");
            // Translate "ERR <Reason>" into an exception
            ETDCASSERT(fields[1].str()=="OK", "sendFile failed - " << fields[2].str());
            // Otherwise we're done
            break;
        }
        return true;
    }

    //////////////////////////////////////////////////////////////////////
    //
    // This class does NOT implementing the ETDServerInterface but
    // takes a connection, instantiates its own ETDServer
    // and then loops, reading commands from the connection and sends
    // back replies
    //
    //////////////////////////////////////////////////////////////////////

    void ETDServerWrapper::handle( void ) {
        // here we enter our while loop, reading commands and (attempt) to
        // interpret them.
        // If we go 2kB w/o seeing an actual command we call it a day
        // I mean, our commands are typically *very* small
        const size_t            bufSz( 2*1024/**1024*/ );
        std::unique_ptr<char[]> buffer(new char[bufSz]);

        bool          terminated = false;
        size_t        curPos = 0;

        while( !terminated && curPos<bufSz ) {
            ETDCDEBUG(5, "ETDServerWrapper::handle() / start loop, curPos=" << curPos << std::endl);
            const ssize_t n = __m_connection->read(__m_connection->__m_fd, &buffer[curPos], bufSz-curPos);
            ETDCDEBUG(5, "ETDServerWrapper::handle() / read n=" << n << " => nTotal=" << n + curPos << std::endl);
            // did we read anything?
            ETDCASSERT(n>0, "Failed to read data from remote end");
            curPos += n;

            // Parse the reply so far
            std::list<std::string> lines;
            std::smatch::size_type endpos = getReplies(&buffer[0], &buffer[curPos], std::back_inserter(lines));
            auto                   line = lines.begin();

            for( ; line!=lines.end(); line++ ) {
                // Got a line! Assert that it conforms to our expectation
                ETDCDEBUG(4, "ETDServerWrapper::handle()/got line: '" << *line << "'" << std::endl);

                // The known commands
                static const std::regex  rxList("^list\\s+(\\S.*)$", etdc_rxFlags);
                static const std::regex  rxReqFileWrite("^write-file-(\\S+)\\s+(\\S.*)$", etdc_rxFlags);
                                                //                   1         2
                                                //                   openmode  file name
                static const std::regex  rxReqFileRead("^read-file\\s+([0-9]+)\\s+(\\S.*)$", etdc_rxFlags);
                                                //                    1           2
                                                //                    already have
                                                //                                file name
                static const std::regex  rxSendFile("^send-file\\s+(\\S+)\\s+(\\S+)\\s+([0-9]+)\\s+(\\S+)$", etdc_rxFlags);
                                                //                 1         2         3           4
                                                //                 srcUUID   dstUUID   todo        data-channel
                static const std::regex  rxDataChannelAddr("^data-channel-addr$", etdc_rxFlags);
                static const std::regex  rxRemoveUUID("^remove-uuid\\s+(\\S+)$", etdc_rxFlags);
                                                //                     1
                                                //                     UUID

                // Match it against the known commands
                std::smatch              fields;
                std::vector<std::string> replies;

                try {
                    if( std::regex_match(*line, fields, rxList) ) {
                        // we're a remote ETDServer (seen from the client)
                        // so we do not support ~ expansion
                        const auto entries = __m_etdserver.listPath(fields[1].str(), false);
                        std::transform(std::begin(entries), std::end(entries), std::back_inserter(replies),
                                       std::bind(std::plus<std::string>(), std::string("OK "), std::placeholders::_1));
                        // and add a final OK
                        replies.emplace_back("OK");
                    } else if( std::regex_match(*line, fields, rxReqFileWrite) ) {
                        openmode_type      om;
                        std::istringstream iss( fields[1].str() );
                        // Transform openmode string to actual openmode enum
                        iss >> om;
                        // Do the actual filewrite request
                        const auto         fwresult = __m_etdserver.requestFileWrite(fields[2].str(), om);
                        std::ostringstream oss;
                        // Prepare replies
                        oss << "AlreadyHave:" << get_filepos(fwresult);
                        replies.emplace_back(oss.str());
                        replies.emplace_back("UUID:"+get_uuid(fwresult));
                        replies.emplace_back("OK");
                    } else if( std::regex_match(*line, fields, rxReqFileRead) ) {
                        // Decode the filepos from the sent command into
                        // local, correctly typed, variable
                        off_t               already_have;;
                        string2off_t(fields[1].str(), already_have);

                        // Do the actual fileread request
                        const auto frresult = __m_etdserver.requestFileRead(fields[2].str(), already_have);

                        // Prepare replies
                        std::ostringstream  oss;
                        oss << "Remain:" << get_filepos(frresult);
                        replies.emplace_back(oss.str());
                        replies.emplace_back("UUID:"+get_uuid(frresult));
                        replies.emplace_back("OK");
                    } else if( std::regex_match(*line, fields, rxSendFile) ) {
                        // Decode the fields 
                        off_t                 todo;
                        const std::string     dataAddrs_s( fields[4].str() );
                        dataaddrlist_type     dataAddrs;
                        const etdc::uuid_type src_uuid{ fields[1].str() };
                        const etdc::uuid_type dst_uuid{ fields[2].str() };

                        string2off_t(fields[3].str(), todo);
                        // transform data channel addresses into list-of-*
                        static const std::regex data_sep("[^,]+");
                        std::transform( std::sregex_iterator(std::begin(dataAddrs_s), std::end(dataAddrs_s), data_sep),
                                        std::sregex_iterator(), std::back_inserter(dataAddrs), 
                                        [](std::smatch const& sm) { return decode_data_addr(sm.str()); });

                        const bool rv = __m_etdserver.sendFile(src_uuid, dst_uuid, todo, dataAddrs);
                        replies.emplace_back( rv ? "OK" : "ERR Failed to send file" );
                    } else if( std::regex_match(*line, fields, rxDataChannelAddr) ) {
                        const auto entries = __m_etdserver.dataChannelAddr();
                        std::transform(std::begin(entries), std::end(entries), std::back_inserter(replies),
                                       [](sockname_type const& sn) { std::ostringstream oss; oss << "OK " << sn; return oss.str(); });
                        // and add a final OK
                        replies.emplace_back("OK");
                    } else if( std::regex_match(*line, fields, rxRemoveUUID) ) {
                        const bool removeResult = __m_etdserver.removeUUID(uuid_type(fields[1].str()));
                        ETDCDEBUG(4, "ETDServerWrapper: removeUUID(" << fields[1].str() << " yields " << removeResult << std::endl);
                        replies.emplace_back( removeResult ? "OK" : "ERR Failed to remove UUID" );
                    } else {
                        ETDCDEBUG(4, "line '" << *line << "' did not match any regex" << std::endl);
                        __m_connection->close( __m_connection->__m_fd );
                        throw std::string("client sent unknown command");
                    }
                }
                catch( std::string const& e ) {
                    ETDCDEBUG(-1, "ETDServerWrapper: terminating because of condition " << e << std::endl);
                    terminated = true;
                }
                catch( std::exception const& e ) {
                    replies.emplace_back( std::string("ERR ")+e.what() );
                }
                catch( ... ) {
                    replies.emplace_back( "ERR Unknown exception" );
                }

                // Now send back the replies
                for(auto const& r: replies) {
                    ETDCDEBUG(4, "ETDServerWrapper: sending reply '" << r << "'" << std::endl);
                    __m_connection->write(__m_connection->__m_fd, r.data(), r.size());
                    __m_connection->write(__m_connection->__m_fd, "\n", 1);
                }
            } 
            ETDCASSERT(line==lines.end(), "There were unprocessed lines of input from the client. This is likely a logical error in this server");
            // Processed all lines in the reply so far.
            // So we move all processed bytes to begin of buffer
            ::memmove(&buffer[0], &buffer[endpos], curPos - endpos);
            curPos -= endpos;
        }
        ETDCDEBUG(3, "ETDServerWrapper: terminated." << std::endl);
    }


    //////////////////////////////////////////////////////////////////////
    //
    //  This class also does NOT implement the ETDServerInterface;
    //  this is the ETDDataServer - it only deals with data connections
    //
    //////////////////////////////////////////////////////////////////////
    static const std::regex rxCommand("^(\\{([^\\}]*)\\})");
    //               subgroup indices:  1   2
    //                                      just the fields
    //                                  whole command

    // we support key:value pairs
    using  kvmap_type = std::map<std::string, std::string, etdc::case_insensitive_lt>;
    static const std::regex rxKeyValue("\\b([a-zA-Z][a-zA-Z0-9_-]+)\\s*:\\s*(\"(.*(?!\\\\))\"|[^, \\t\\v]+)", etdc_rxFlags);
    //             subgroup indices:       1                          2   3 quoted string literal
    //                                     key                        complete value 
    // NOTE: in the rxKeyValue we write out [a-zA-Z][a-zA-Z...] despite
    //       etdc_rxFlags contains std::regex::icase. Turns out GCC libstdc++ 
    //       has a bugz - 'case insensitive match' on character range(s)
    //       don't work!
    //          https://gcc.gnu.org/bugzilla/show_bug.cgi?id=71500
    //       At the time of writing this (29 Jun 2017) this was not yet
    //       fixed in the gcc version(s) that I had access to.
    static const std::regex rxSlash("\\\\");
    static const auto       unSlash = [](std::string const& s) { return std::regex_replace(s, rxSlash, ""); };

    template <typename InputIter, typename OutputIter,
              typename RegexIter  = std::regex_iterator<InputIter>,
              typename RetvalType = typename std::match_results<InputIter>::size_type>
    static RetvalType getKeyValuePairs(InputIter f, InputIter l, OutputIter o) {
        static const RegexIter  rxIterEnd{};

        RetvalType endpos = 0;
        for(RegexIter kv = RegexIter(f, l, rxKeyValue); kv!=rxIterEnd; kv++) {
            // Found another key-value pair: append to output and update endpos
            const auto this_kv = *kv;
            *o++   = kvmap_type::value_type{ this_kv[1].str(), unSlash((this_kv[3].length() ? this_kv[3].str() : this_kv[2].str())) };
            endpos = this_kv.position() + this_kv.length();
        }
        return endpos;
    }

    void ETDDataServer::handle( void ) {
        // When writing to a file these are the allowed modes
        static const std::set<openmode_type> allowedWriteModes{openmode_type::New, openmode_type::OverWrite, openmode_type::Resume};
        static const std::set<openmode_type> allowedReadModes{openmode_type::Read};

        // here we enter our while loop, reading commands and (attempt) to
        // interpret them.
        // If we go 2kB w/o seeing an actual command we call it a day
        // I mean, our commands are typically *very* small
        const size_t            maxNoCmdSz( 4*1024 );
        const size_t            bufSz( 10*1024*1024 );
        std::unique_ptr<char[]> buffer(new char[bufSz]);

        bool          terminated = false;
        size_t        curPos = 0;

        // Read at most maxNoCmdSz bytes to see if there is a command
        // embedded. If not, then we assume the client is broken or trying
        // to break us so we just terminate
        while( !terminated && curPos<maxNoCmdSz ) {
            ETDCDEBUG(5, "ETDDataServer::handle() / start loop, curPos=" << curPos << std::endl);
            const ssize_t n = __m_connection->read(__m_connection->__m_fd, &buffer[curPos], maxNoCmdSz-curPos);
            ETDCDEBUG(5, "ETDDataServer::handle() / read n=" << n << " => nTotal=" << n + curPos << std::endl);
            // did we read anything?
            ETDCASSERT(n>0, "Failed to read data from remote end");
            curPos += n;

            // We know that we have a non-zero amount of bytes read from the client.
            // If the first byte is not '{' then we're screwed
            ETDCASSERT(buffer[0]=='{', "Client is messing with us - doesn't look like it is going to send a command");

            // If we end up here we're looking for commands:
            // '{ uuid:.... , sz: ..., [push: 1, data_addr: ....] }' + binary data
            kvmap_type             kvpairs;
            std::cmatch            command;

            if( !std::regex_search((const char*)&buffer[0], (const char*)&buffer[curPos], command, rxCommand) ) {
                ETDCDEBUG(4, "ETDDataServer: so far no command in bytes 0.." << curPos << std::endl);
                continue;
            }
            // OK we found "{ ... }" in the current buffer
            ETDCDEBUG(4, "ETDDataServer: found command @" << command.position() << " + " << command.length() << std::endl);

            // Ignore the return value of getKeyValuePairs - we already
            // have the command match doing that for us
            (void)getKeyValuePairs(&buffer[command.position() + 1], &buffer[command.position() + command.length() - 1],
                                   etdc::no_duplicates_inserter(kvpairs, kvpairs.end()));

            ETDCDEBUG(4, "ETDDataServer: found " << kvpairs.size() << " key-value pairs inside:" << std::endl);
            for(const auto& kv: kvpairs)
                ETDCDEBUG(4, "   " << kv.first << ":" << kv.second << std::endl);

            // By the time we get here, we know for sure:
            //  1.) there was a command '{ ... }' in our buffer
            //  2.From: ) it may have had a number of key-value pairs in there
            //
            // Now it's time to verify:
            //  - we need 'uuid:'  and 'sz:' key-value pairs
            //  - there may be 'push:1' 
            off_t      sz;
            const auto uuidptr = kvpairs.find("uuid");
            const auto szptr   = kvpairs.find("sz");
            const auto pushptr = kvpairs.find("push");

            ETDCASSERT(uuidptr!=kvpairs.end(), "No UUID was sent");
            ETDCASSERT(szptr!=kvpairs.end(), "No amount was sent");
            ETDCASSERT(pushptr==kvpairs.end() || pushptr->second=="1", "push keyword may only take one specific value");
            // The size must be an off_t value
            string2off_t(szptr->second, sz);

            // Verification = complete.
            // Now we must grab a lock on the transfer (if there is one)
            // and do our thang
            const bool                       push = (pushptr!=kvpairs.end());
            etdc::etd_state&                 shared_state( __m_shared_state.get() );
            std::unique_lock<std::mutex>     xfer_lock;
            etdc::transfermap_type::iterator xfer_ptr;

            // Loop until we've got the lock acquired
            while( !xfer_lock.owns_lock() /*true*/ ) {
                // 2a. lock shared state
                std::unique_lock<std::mutex>     lk( shared_state.lock );
                // 2b. assert that there is an entry for the indicated uuid
                xfer_ptr = shared_state.transfers.find(uuid_type(uuidptr->second));

                ETDCASSERT(xfer_ptr!=shared_state.transfers.end(), "No transfer associated with the UUID");

                // Now we must do try_lock on the transfer - if that fails we sleep and start from the beginning
                std::unique_lock<std::mutex>     sh( xfer_ptr->second->lock, std::try_to_lock );
                if( !sh.owns_lock() ) {
                    // Manually unlock the shared state or else nobody won't be
                    // able to change anything!
                    lk.unlock();

                    // *now* we sleep for a bit and then try again
                    std::this_thread::sleep_for( std::chrono::microseconds(9) );
                    //std::this_thread::sleep_for( std::chrono::seconds(5) );
                    continue;
                }
                // Technically we could've tested the following /before/ getting a
                // lock on the transfer; we're only checking the transfer's
                // properties to make sure it is compatible with the current
                // request.
                // But putting the test in before attempting to lock the
                // transfer would mean that we would be doing this test over
                // and over again until we actually managed to lock the
                // transfer, which sounds a bit wasteful.
                // So now we test it once, after we've acquired the lock
                ETDCASSERT( (push ? allowedReadModes.find(xfer_ptr->second->openMode)!=allowedReadModes.end() :
                                    allowedWriteModes.find(xfer_ptr->second->openMode)!=allowedWriteModes.end()),
                            "The referred-to transfer's open mode (" << xfer_ptr->second->openMode << ") is not compatible with the current data request");
                // move the transfer lock out of this loop;
                // breaking out of the loop will unlock the shared state
                xfer_lock = std::move( sh );
            }
            ETDCDEBUG(5, "ETDDataServer/owning transfer lock, now sucking data!" << std::endl);

            // If we end up here we know that the transfer is locked and
            // that xfer_ptr is pointing at it and that all is good
            // Now defer to appropriate subordinate fn
            
            // We found a valid command in the buffer, there may be raw bytes left following that command.
            // Therefore we initialize our read position to the end of the command we found.
            const size_t  rdPos( command.position() + command.length() ); 
            if( push )
                ETDDataServer::push_n(sz, xfer_ptr->second->fd, __m_connection, rdPos, curPos, bufSz, buffer);
            else
                ETDDataServer::pull_n(sz, __m_connection, xfer_ptr->second->fd, rdPos, curPos, bufSz, buffer);
            // This command has been served, ready to accept next
            curPos = 0;
        }
        ETDCDEBUG(4, "ETDDataServer::handle() / terminated" << std::endl);
    }

    // PUSH n bytes src to dst, using buffer of size bufSz.
    // the bytes between endPos and rdPos is are what was read from the
    // client, following the command. But since we're pushing we're going to 
    // ignore any extra bytes sent by the client and overwrite everything in
    // the buffer
    void ETDDataServer::push_n(size_t n, etdc::etdc_fdptr& src, etdc::etdc_fdptr& dst,
                               size_t /*rdPos*/, const size_t /*endPos*/, const size_t bufSz, std::unique_ptr<char[]>& buf) {
        ETDCDEBUG(5, "ETDDataServer::push_n/n=" << n << std::endl);
        while( n>0 ) {
            // Amount of bytes to process in this iteration
            const ssize_t nRead = std::min(n, bufSz);
            ETDCDEBUG(5, "ETDDataServer::push_n/iteration/nRead=" << nRead << std::endl);

            ETDCASSERTX(src->read(src->__m_fd,  &buf[0], nRead)==nRead);
            ETDCASSERTX(dst->write(dst->__m_fd, &buf[0], nRead)==nRead);
            n -= nRead;
        }
        // Do a read from the destination such that we know it is finished
        char ack;
        ETDCDEBUG(5, "ETDDataServer::push_n/waiting for ACK " << std::endl);
        dst->read(dst->__m_fd, &ack, 1);
        ETDCDEBUG(5, "ETDDataServer::push_n/done." << std::endl);
    }
    // PULL n bytes from rc to dst, using buffer of size bufSz
    // the bytes between endPos and rdPos are what was read from the client,
    // raw bytes immediately following the command. We flush those to the
    // file first and then we can use the whole buffer for reading bytes.
    void ETDDataServer::pull_n(size_t n, etdc::etdc_fdptr& src, etdc::etdc_fdptr& dst,
                               size_t rdPos, const size_t endPos, const size_t bufSz, std::unique_ptr<char[]>& buf) {
        // rdPos:  current start of read area in buf
        // endPos: passed in from above; this is where the initial command
        //         reader left off
        // wrPos:  current end of read aread in buf
        // bufSz:  size of buf
        size_t  wrEnd( endPos );
        ETDCDEBUG(5, "ETDDataServer::pull_n/n=" << n << " rdPos=" << rdPos << " wrEnd=" << wrEnd << std::endl);
        while( n>0 ) {
            // Attempt read as many bytes into our buffer as we can; there
            // should be room for bufSz - wrEnd bytes. Amount of bytes still/already in buf = wrEnd - rdPos
            // (thus: "n - (wrEnd - rdPos)" amount still to be read, if any; and "n - (wrEnd - rdPos)" == "n + rdPos - wrEnd"
            ssize_t       aRead;
            const ssize_t nRead = std::min(n + rdPos - wrEnd, bufSz - wrEnd);
        
            ETDCDEBUG(5, "ETDDataServer::pull_n/iteration/nRead=" << nRead << std::endl);

            // Attempt to read bytes. <0 is an error
            ETDCASSERT((aRead = src->read(src->__m_fd, &buf[wrEnd], nRead))>=0, "Failed to read bytes from client - " << etdc::strerror(errno));

            // Now we can bump wrEnd by that amount [at this point aRead might still be zero]
            wrEnd += aRead;

            // If there are no bytes to write to file that means that 0
            // bytes were read and no bytes still left in buffer == error
            ETDCASSERT((wrEnd - rdPos)>0, "No bytes read from client and no more bytes still left in buffer");

            // Now flush the amount of available bytes to the destination
            ETDCASSERTX(dst->write(dst->__m_fd, &buf[rdPos], wrEnd-rdPos)==ssize_t(wrEnd-rdPos));

            n -= (wrEnd - rdPos);

            // Now we are sure we can use the whole buffer for reading bytes
            // from the client
            wrEnd = rdPos = 0;
        }
        const char ack{ 'y' };
        ETDCDEBUG(5, "ETDDataServer::pull_n/got all bytes, sending ACK " << std::endl);
        src->write(src->__m_fd, &ack, 1);
        ETDCDEBUG(5, "ETDDataServer::pull_n/done." << std::endl);
    }

} // namespace etdc