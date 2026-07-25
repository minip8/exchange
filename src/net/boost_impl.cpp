/*
The single translation unit carrying the Asio / Beast / JSON implementation.

net_boost defines BOOST_ASIO_SEPARATE_COMPILATION and
BOOST_BEAST_SEPARATE_COMPILATION for every net TU, which turns those headers
into declarations only. This file is where the definitions land. Keep it
otherwise empty — anything added here gets recompiled at the cost of the
whole of Asio.
*/
#include <boost/asio/impl/src.hpp>
#include <boost/beast/src.hpp>
#include <boost/json/src.hpp>
