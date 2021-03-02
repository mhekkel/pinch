//           Copyright Maarten L. Hekkelman 2013
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <pinch/pinch.hpp>
#include <pinch/error.hpp>

using namespace std;

namespace pinch {
namespace error {
namespace detail {

class ssh_category : public boost::system::error_category
{
  public:

	const char* name() const BOOST_SYSTEM_NOEXCEPT
	{
		return "ssh";
	}
	
	std::string message(int value) const 
	{
		switch (value)
		{
			case error::unimplemented:
				return "Unimplemented SSH call";
			case error::userauth_failure:
				return "User authentication failure";
			case error::request_failure:
				return "SSH request failure";
			case error::channel_open_failure:
				return "Failed to open SSH channel";
			case error::channel_failure:
				return "SSH channel failure";

			case error::host_key_verification_failed:
				return "SSH host key not verified";
			case error::channel_closed:
				return "SSH channel closed";
			case error::require_password:
				return "Password requested";
			case error::not_authenticated:
				return "session not authenticated yet";
			case error::disconnect_by_host:
				return "connection closed by host";

			default:
				return "ssh error";
		}
	}
};

class disconnect_category : public boost::system::error_category
{
  public:

	const char* name() const BOOST_SYSTEM_NOEXCEPT
	{
		return "ssh.disconnect";
	}
	
	std::string message(int value) const
	{
		switch (value)
		{
			case error::host_not_allowed_to_connect:
				return "Host is not allowed to connect";
			case error::protocol_error:
				return "SSH Protocol error";
			case error::key_exchange_failed:
				return "SSH key exchange failed";
			case error::reserved:
				return "SSH reserved error";
			case error::mac_error:
				return "SSH message authentication error";
			case error::compression_error:
				return "SSH compression error";
			case error::service_not_available:
				return "SSH service not available";
			case error::protocol_version_not_supported:
				return "SSH protocol version not supported";
			case error::host_key_not_verifiable:
				return "SSH host key not verifiable";
			case error::connection_lost:
				return "SSH connection lost";
			case error::by_application:
				return "SSH error generated by application";
			case error::too_many_connections:
				return "SSH too many connections";
			case error::auth_cancelled_by_user:
				return "SSH authentication cancelled by user";
			case error::no_more_auth_methods_available:
				return "No more authentication methods available";
			case error::illegal_user_name:
				return "Illegal user name";
			default:
				return "ssh disconnected with error";
		}
	}
};

}

boost::system::error_category& ssh_category()
{
	static detail::ssh_category impl;
	return impl;
}

boost::system::error_category& disconnect_category()
{
	static detail::disconnect_category impl;
	return impl;
}

}

}
