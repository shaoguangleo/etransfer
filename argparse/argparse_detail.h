// Support utilities like tests and types
#ifndef ARGPARSE_DETAIL_H
#define ARGPARSE_DETAIL_H

#include <argparse_functools.h>

#include <tuple>
#include <memory>
#include <sstream>
#include <iostream>
#include <type_traits>

//#include <ctype.h>
#include <cstdlib>   // for std::free, std::exit
//#include <cstring>
#include <cxxabi.h>

namespace argparse { namespace detail {

    ////////////////////////////////////////////////////////////////////////////
    //
    //  C++11 has std::ignore
    //          http://en.cppreference.com/w/cpp/utility/tuple/ignore
    //
    //  So why not follow that in case WE want to indicate that something
    //  is to be ignored
    //
    ////////////////////////////////////////////////////////////////////////////
    using ignore_t = typename std::decay<decltype(std::ignore)>::type;

    ////////////////////////////////////////////////////////////////////////////
    //
    //        Tests if "std::ostream<<T" is well-formed
    //
    ////////////////////////////////////////////////////////////////////////////
    template <typename T>
    struct is_streamable {
        using yes = char;
        using no  = long long int;

        template <typename U>
        static auto test(std::ostream& os, U const& u) -> decltype(os << u, yes()) {}
        //                                                ^^^^^^^^^^^^^^^^^^^^^^^^
        //                                 Thanks @ http://stackoverflow.com/a/9154394

        template <typename U>
        static no test(...);

        static constexpr bool value = (sizeof(test<T>(std::declval<std::ostream&>(), std::declval<T const&>()))==sizeof(yes));
    };

    ////////////////////////////////////////////////////////////////////////////
    //
    // Tests if T has "Ret operator()(Args...)" defined
    // (yes, also checks the return type)
    //
    // The Match<...> is a template template parameter so user can
    // influence the match type (e.g. exact or convertible-to, see below)
    //
    ////////////////////////////////////////////////////////////////////////////
    template <template <typename...> class Match, typename T, typename Ret, typename... Args>
    struct has_operator {
        using yes = char;
        using no  = unsigned long;

        // Test if V matches the requested Ret according to Match specification
        template <typename V,
                  typename std::enable_if<Match<V, Ret>::value, int>::type = 0>
        static yes test_rv(V*);

        template <typename V>
        static no  test_rv(...);

        // Test if T has "operator()(Args...)" defined in the first place
        // (by testing if calling it is well-formed)
        template <typename U>
        static auto test( decltype( std::declval<U>()(std::declval<Args>()...) )* ) -> 
            // Yes, proceed to test the return type
            decltype( test_rv<decltype(std::declval<U>()(std::declval<Args>()...))>(nullptr) );
        template <typename U>
        static no   test(...);

        static constexpr bool value = sizeof(test<T>(nullptr))==sizeof(yes);
    };

    ////////////////////////////////////////////////////////////////////////////
    //
    //  The canned tests do an exact or convertible-to match on the
    //  requested operator
    //
    ////////////////////////////////////////////////////////////////////////////
    template <typename... Ts>
    using has_compatible_operator = has_operator<std::is_convertible, Ts...>;
    template <typename... Ts>
    using has_exact_operator      = has_operator<std::is_same, Ts...>;


    namespace detail {
        // Adaptor for easy creation of reversed range-based for loop, thanks to:
        // http://stackoverflow.com/a/36928761
        template <typename C>
        struct reverse_wrapper {
            C& c_;   // could use std::reference_wrapper, maybe?
            reverse_wrapper(C& c) :  c_(c) {}

            // container.rbegin()/container.rend() is fine - these have existed for long
            typename C::reverse_iterator begin() { return c_.rbegin(); }
            typename C::reverse_iterator end()   { return c_.rend();   }
        };

        template <typename C, size_t N>
        struct reverse_wrapper< C[N] >{
            C (&c_)[N];
            reverse_wrapper( C(&c)[N] ) : c_(c) {}

            // std::rbegin()/std::rend() are c++14 :-(
            typename std::reverse_iterator<const C *> begin() { return &c_[N-1];/*std::rbegin(c_);*/ }
            typename std::reverse_iterator<const C *> end()   { return &c_[-1]; /*std::rend(c_);*/   }
        };
    }

    ////////////////////////////////////////////////////////////////////////////
    //
    //   Container adaptor for range-based for loops that allow for
    //   iterating in reverse order over the container
    //
    ////////////////////////////////////////////////////////////////////////////
    template <typename C>
    detail::reverse_wrapper<C> reversed(C& c) {
        return detail::reverse_wrapper<C>(c);
    }



    ////////////////////////////////////////////////////////////////////////////
    //
    // Tests if type C has typedef "iterator" , typedef "value_type" and a
    // member function ".insert(iterator, value_type)"
    //
    // Which should be enough duck-typing to test for any std container that
    // supports insertion of a value at any iterator position hint
    //
    ////////////////////////////////////////////////////////////////////////////
    template <typename T> struct can_insert {
        // Only works if sizeof(char)!=sizeof(unsigned long)
        using yes = char; using no  = unsigned long;


        // Test if ".insert(iterator, value)" is well-defined
        template <typename Container, typename Value>
        static auto test2(Container* w) -> decltype(w->insert(w->end(), std::declval<Value>()), yes());

        template <typename Container, typename Value>
        static no   test2(...);

        // Test for existance of the member typedefs ::value_type and ::iterator
        template <typename Container>
        static auto test(typename Container::value_type* v, typename Container::iterator*) ->
                // Yes, now check if there's ".insert(...)"
                decltype( test2<Container, typename Container::value_type>(nullptr) );

        template <typename Container>
        static no   test(...);

        static constexpr bool value = sizeof(test<T>(nullptr, nullptr))==sizeof(yes);
    };


    ////////////////////////////////////////////////////////////////////////////
    //
    // Deduce the return type and arguments of a callable
    //
    // This implementation is a combination of tricks found here:
    //      https://github.com/Manu343726/TTL/blob/master/include/overloaded_function.hpp
    //      http://stackoverflow.com/a/21665705
    //
    //  NOTE: due to the nature of how std::bind(...) is implemented this
    //        code cannot accept instances of std::bind(...) - the function
    //        call signature of a std::bind(...) object can not be inferred:
    //        see http://stackoverflow.com/a/21739025
    //
    ////////////////////////////////////////////////////////////////////////////
    template <typename R, typename... Args>
    struct signature {
        using return_type                  = R;
        using argument_type                = std::tuple<Args...>;
        static constexpr std::size_t arity = sizeof...(Args);
    };

    template <typename T>
    struct deduce_signature: deduce_signature< decltype( &T::operator() ) > {
        static_assert( !std::is_bind_expression<T>::value,
                       "The signature of a std::bind(...) expression can not be inferred. Wrap your call in a lambda instead.");
    };

    template <typename R, typename... Args>
    struct deduce_signature<R(*)(Args...)>: signature<R, Args...> {};

    template <typename R, typename... Args>
    struct deduce_signature<R(&)(Args...)>: signature<R, Args...> {};

    template <typename R, typename... Args>
    struct deduce_signature<std::function<R(Args...)>> : signature<R, Args...> {};

    // these two capture lambda's - they have a scope!
    template <typename R, typename C, typename... Args>
    struct deduce_signature<R(C::*)(Args...)> : signature<R, Args...> {};

    template <typename R, typename C, typename... Args>
    struct deduce_signature<R(C::*)(Args...) const> : signature<R, Args...> {};

    template <typename R, typename C, typename... Args>
    struct deduce_signature<std::function<R(C::*)(Args...)>> : signature<R, Args...> {};

    template <typename R, typename C, typename... Args>
    struct deduce_signature<std::function<R(C::*)(Args...) const>> : signature<R, Args...> {};

    template <typename T>
    struct is_unary_fn: std::integral_constant<bool, T::arity==1> {};


    ////////////////////////////////////////////////////////////////////////////
    //
    // Kwik-n-dirty / 'lightweight type categorization idiom'
    // Thanks to http://stackoverflow.com/a/9644512
    //
    // Use it to get a quick test if a type seems to be std:: container;
    // all of them have "::value_type"
    //
    ////////////////////////////////////////////////////////////////////////////
    template<class T, class R = void>  
    struct enable_if_type { typedef R type; };

    template<class T, class Enable = void>
    struct maybe_container : std::false_type {};

    template<class T>
    struct maybe_container<T, typename enable_if_type<typename T::value_type>::type> : std::true_type {};



    ////////////////////////////////////////////////////////////////////////////
    //
    // Demangle a typename into human readable form
    //
    // Note: Should look at http://stackoverflow.com/a/20303333
    //
    ////////////////////////////////////////////////////////////////////////////
    template <typename T>
    std::string demangle_f(void) {
        int              status = -4; // some arbitrary value to eliminate the compiler warning
        char const*const name = typeid(T).name();

        // enable c++11 by passing the flag -std=c++11 to g++
        std::unique_ptr<char, void(*)(void*)> res {
            abi::__cxa_demangle(name, NULL, NULL, &status),
            std::free
        };

        return (status==0) ? res.get() : name ;
    }


    ////////////////////////////////////////////////////////////////////////////
    //  For usage we don't want "std::string" printed
    //  so we'll overload the demangling of std::string to just "string"
    ////////////////////////////////////////////////////////////////////////////
    template <typename T>
    std::string optiontype( void ) {
        return demangle_f<T>();
    }
    template <>
    std::string optiontype<std::string>( void ) {
        return "string";
    }


    ////////////////////////////////////////////////////////////////////////////
    //
    // Convert a value/type to string representation.
    //
    // For a value that supports operator<< we use that to construct a
    // string representing the value and return that.
    //
    // For things that don't support operator<<, we output the (demangled) type
    // name. 
    //
    // For some types we don't have have to go through operator<< in order
    // to get the string representation (e.g. strings themselves ...)
    //
    ////////////////////////////////////////////////////////////////////////////
    struct string_repr {
        // If it's streamable, then we insert its value
        template <typename T,
                  typename std::enable_if<is_streamable<T>::value, int>::type = 0>
        std::string operator()(T const& t) const {
            std::ostringstream oss;
            oss << t;
            return oss.str();
        }
        // Otherwise we just output the (demangled) type name
        template <typename T,
                  typename std::enable_if<!is_streamable<T>::value, int>::type = 0>
        std::string operator()(T const&) const {
            return demangle_f< typename std::decay<T>::type >();
        }

        // Some specializations that don't have to go through "operator<<"
        std::string operator()(std::string const& s) const {
            return s;
        }
        std::string operator()(char const& c) const {
            return std::string(&c, &c+1);
        }
        std::string operator()(char const*const c) const {
            return std::string(c);
        }
    };

    ////////////////////////////////////////////////////////////////////////////
    //
    // Std operators as readable strings
    //
    // This works on the template template parameters :D
    // So you don't have to instantiate e.g. "std::less<int>" to get this;
    // "op2str<std::less>()" gives the desired string representation 
    //
    ////////////////////////////////////////////////////////////////////////////
    template <template <typename...> class OP>
    std::string op2str( void ) {
        return "<unknown operator>";
    }
    template <>
    std::string op2str<std::less>( void ) {
        return "less than";
    }
    template <>
    std::string op2str<std::less_equal>( void ) {
        return "less than or equal";
    }
    template <>
    std::string op2str<std::greater>( void ) {
        return "greater than";
    }
    template <>
    std::string op2str<std::greater_equal>( void ) {
        return "greater than or equal";
    }
    template <>
    std::string op2str<std::equal_to>( void ) {
        return "equal to";
    }


    ////////////////////////////////////////////////////////////////////////////
    //
    //  Nifty varargs style string builder.
    //  Transforms its varargs to tuple and uses functools to map the
    //  "string_repr" functor over all the elements and then uses
    //  functools again to foldl ('reduce') all the formed strings into a
    //  single string and returns that.
    //
    ////////////////////////////////////////////////////////////////////////////
    template <typename... Ts>
    std::string build_string(Ts&&... ts) {
        return functools::foldl(std::plus<std::string>(),
                                functools::map(std::forward_as_tuple(ts...), string_repr()),
                                std::string());
    }

} } // namespace argparse { namespace detail {

#endif
