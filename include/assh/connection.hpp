//           Copyright Maarten L. Hekkelman 2013
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <assh/config.hpp>
#include <assh/error.hpp>
#include <assh/key_exchange.hpp>

#include <boost/algorithm/string.hpp>
#include <boost/iostreams/filtering_stream.hpp>

#include <list>
#include <functional>

#include <assh/packet.hpp>
#include "assh/crypto-engine.hpp"
#include "assh/ssh_agent.hpp"


namespace assh
{

// --------------------------------------------------------------------

class socket_closed_exception : public exception
{
  public:
	socket_closed_exception() : exception("socket is closed") {}
};

class key_exchange;

template<typename> class channel;
template<typename S> using channel_ptr = std::shared_ptr<channel<S>>;

class port_forward_listener;

// --------------------------------------------------------------------

extern const std::string kSSHVersionString;

template<typename> class basic_connection;

// keyboard interactive support
struct prompt
{
	std::string str;
	bool echo;
};
typedef std::function<void(const std::string &, const std::string &, const std::vector<prompt> &)> keyboard_interactive_callback_type;

// --------------------------------------------------------------------

class connection_base
{
  public:
	virtual ~connection_base() {}

	virtual bool is_connected() const = 0;
	virtual void disconnect() = 0;

	virtual void async_write(opacket&& p) = 0;

	virtual bool uses_private_key(const std::vector<uint8_t>& pk_hash) = 0;
};


namespace detail
{


enum class auth_state_type
{
	none,
	connecting,
	public_key,
	keyboard_interactive,
	password,
	connected,
	authenticated
};

template<typename Stream>
struct async_connect_impl
{
	using socket_type = Stream;

	socket_type& socket;
	boost::asio::streambuf& response;
	std::shared_ptr<basic_connection<Stream>> conn;
	std::string user;

	enum state_type { start, wrote_version, reading, rekeying, authenticating } state = start;
	auth_state_type auth_state = auth_state_type::none;

	std::string host_version;
	std::unique_ptr<boost::asio::streambuf> request = std::make_unique<boost::asio::streambuf>();
	std::unique_ptr<ipacket> packet = std::make_unique<ipacket>();
	std::unique_ptr<key_exchange> kex;
	std::deque<opacket> private_keys;
	std::vector<uint8_t> private_key_hash;

	keyboard_interactive_callback_type m_request_password_cb, m_keyboard_interactive_cb;
	int m_password_attempts = 0;

	template<typename Self>
	void operator()(Self& self, boost::system::error_code ec = {}, std::size_t bytes_transferred = 0);

	void process_userauth_success(ipacket &in, opacket &out, boost::system::error_code &ec);
	void process_userauth_failure(ipacket &in, opacket &out, boost::system::error_code &ec);
	void process_userauth_banner(ipacket &in);
	void process_userauth_info_request(ipacket &in, opacket &out, boost::system::error_code &ec);
};


template<typename Stream>
template<typename Self>
void async_connect_impl<Stream>::operator()(Self& self, boost::system::error_code ec, std::size_t bytes_transferred)
{
	if (not ec)
	{
		switch (state)
		{
			case start:
			{
				std::ostream out(request.get());
				out << kSSHVersionString << "\r\n";
				state = wrote_version;
				boost::asio::async_write(socket, *request, std::move(self));
				return;
			}
			
			case wrote_version:
				state = reading;
				boost::asio::async_read_until(socket, response, "\n", std::move(self));
				return;

			case reading:
			{
				std::istream response_stream(&response);
				std::getline(response_stream, host_version);
				while (std::isspace(host_version.back()))
					host_version.pop_back();

				if (host_version.substr(0, 7) != "SSH-2.0")
				{
					self.complete(error::make_error_code(error::protocol_version_not_supported));
					return;
				}

				state = rekeying;
				kex = std::make_unique<key_exchange>(host_version);

				conn->async_write(kex->init());
				
				boost::asio::async_read(socket, response, boost::asio::transfer_at_least(8), std::move(self));
				return;
			}

			case rekeying:
			{
				for (;;)
				{
					if (not conn->receive_packet(*packet, ec) and not ec)
					{
						boost::asio::async_read(socket, response, boost::asio::transfer_at_least(1), std::move(self));
						return;
					}

					opacket out;
					if (*packet == msg_newkeys)
					{
						conn->newkeys(*kex, ec);

						if (ec)
						{
							self.complete(ec);
							return;
						}

						state = authenticating;

						out = msg_service_request;
						out << "ssh-userauth";

						// we might not be known yet
						// ssh_agent::instance().register_connection(conn);

						// fetch the private keys
						for (auto& pk: ssh_agent::instance())
						{
							opacket blob;
							blob << pk;
							private_keys.push_back(blob);
						}
					}
					else if (not kex->process(*packet, out, ec))
					{
						self.complete(error::make_error_code(error::key_exchange_failed));
						return;
					}

					if (ec)
					{
						self.complete(ec);
						return;
					}

					if (out)
						conn->async_write(std::move(out));
					
					packet->clear();
				}
			}

			case authenticating:
			{
				if (not conn->receive_packet(*packet, ec) and not ec)
				{
					boost::asio::async_read(socket, response, boost::asio::transfer_at_least(1), std::move(self));
					return;
				}

				auto& in = *packet;
				opacket out;

				switch ((message_type)in)
				{
					case msg_service_accept:
						out = msg_userauth_request;
						out << user << "ssh-connection"
							<< "none";
						break;

					case msg_userauth_failure:
						process_userauth_failure(in, out, ec);
						break;

					case msg_userauth_banner:
						process_userauth_banner(in);
						break;

					case msg_userauth_info_request:
						process_userauth_info_request(in, out, ec);
						break;

					case msg_userauth_success:
						conn->userauth_success(host_version, kex->session_id());
						self.complete({});
						return;
				}

				if (out)
					conn->async_write(std::move(out));
				
				if (ec)
					self.complete(ec);
				else
				{
					packet->clear();
					boost::asio::async_read(socket, response, boost::asio::transfer_at_least(1), std::move(self));
				}

				return;
			}
		}
	}

	self.complete(ec);
}

template<typename Stream>
void async_connect_impl<Stream>::async_connect_impl::process_userauth_failure(ipacket &in, opacket &out, boost::system::error_code &ec)
{
	std::string s;
	bool partial;

	in >> s >> partial;

	private_key_hash.clear();

	if (choose_protocol(s, "publickey") == "publickey" and not private_keys.empty())
	{
		out = opacket(msg_userauth_request)
				<< user << "ssh-connection"
				<< "publickey" << false
				<< "ssh-rsa" << private_keys.front();
		private_keys.pop_front();
		auth_state = auth_state_type::public_key;
	}
	else if (choose_protocol(s, "keyboard-interactive") == "keyboard-interactive" and m_keyboard_interactive_cb and ++m_password_attempts <= 3)
	{
		out = opacket(msg_userauth_request)
				<< user << "ssh-connection"
				<< "keyboard-interactive"
				<< "en"
				<< "";
		auth_state = auth_state_type::keyboard_interactive;
	}
	else if (choose_protocol(s, "password") == "password" and m_request_password_cb and ++m_password_attempts <= 3)
		assert(false);
		// m_request_password_cb();
	else
		ec = error::make_error_code(error::no_more_auth_methods_available);
}

template<typename Stream>
void async_connect_impl<Stream>::process_userauth_banner(ipacket &in)
{
	std::string msg, lang;
	in >> msg >> lang;

std::cerr << msg << '\t' << lang << std::endl;

	// for (auto h : m_connect_handlers)
	// 	h->handle_banner(msg, lang);
}

template<typename Stream>
void async_connect_impl<Stream>::process_userauth_info_request(ipacket &in, opacket &out, boost::system::error_code &ec)
{
	switch (auth_state)
	{
		case auth_state_type::public_key:
		{
			out = msg_userauth_request;

			std::string alg;
			ipacket blob;

			in >> alg >> blob;

			out << user << "ssh-connection"
				<< "publickey" << true << "ssh-rsa" << blob;

			opacket session_id;
			session_id << kex->session_id();

			ssh_private_key pk(ssh_agent::instance().get_key(blob));

			out << pk.sign(session_id, out);

			// store the hash for this private key
			private_key_hash = pk.get_hash();
			break;
		}
		case auth_state_type::keyboard_interactive:
		{
			std::string name, instruction, language;
			int32_t numPrompts = 0;

			in >> name >> instruction >> language >> numPrompts;

			if (numPrompts == 0)
			{
				out = msg_userauth_info_response;
				out << numPrompts;
			}
			else
			{
				std::vector<prompt> prompts(numPrompts);

				for (auto& p : prompts)
					in >> p.str >> p.echo;

				if (prompts.empty())
					prompts.push_back({ "iets", true });

				m_keyboard_interactive_cb(name, language, prompts);
			}
			break;
		}
		default:;
	}
}

}

template<typename Stream>
class basic_connection : public connection_base, public std::enable_shared_from_this<basic_connection<Stream>>
{
  public:

	using channel_type = channel<Stream>;
	using channel_ptr = channel_ptr<Stream>;

	template<typename Arg>
	basic_connection(Arg&& arg, const std::string& user)
		: m_next_layer(std::forward<Arg>(arg))
		, m_user(user)
	{
		reset();
	}

	virtual ~basic_connection()
	{

	}

	/// The type of the next layer.
	using next_layer_type = std::remove_reference_t<Stream>;

	/// The type of the lowest layer.
	using lowest_layer_type = typename next_layer_type::lowest_layer_type;

	/// The type of the executor associated with the object.
	using executor_type = typename lowest_layer_type::executor_type;

	executor_type get_executor() noexcept
	{
		return m_next_layer.lowest_layer().get_executor();
	}

	const next_layer_type& next_layer() const
	{
		return m_next_layer;
	}

	next_layer_type& next_layer()
	{
		return m_next_layer;
	}

	const lowest_layer_type& lowest_layer() const
	{
		return m_next_layer.lowest_layer();
	}

	lowest_layer_type& lowest_layer()
	{
		return m_next_layer.lowest_layer();
	}

	// callbacks to be installed by owning object

	// bool validate_host_key(host, alg, key)
	using validate_callback_type = std::function<bool(const std::string&, const std::string&, const std::vector<uint8_t>&)>;

	// void request_password()
	typedef std::function<void()> password_callback_type;

	void set_validate_callback(const validate_callback_type &cb)
	{
		m_validate_host_key_cb = cb;
	}

	void set_password_callback(const password_callback_type &cb)
	{
		m_request_password_cb = cb;
	}

	void set_keyboard_interactive_callback(const keyboard_interactive_callback_type &cb)
	{
		m_keyboard_interactive_cb = cb;
	}

	virtual bool is_connected() const override
	{
		return m_auth_state == detail::auth_state_type::authenticated;
	}

	using async_connect_impl = detail::async_connect_impl<next_layer_type>;
	friend async_connect_impl;

	template<typename Handler>
	auto async_connect(Handler&& handler)
	{
		return boost::asio::async_compose<Handler, void(boost::system::error_code)>(
			async_connect_impl{
				m_next_layer, m_response, this->shared_from_this(), m_user
			}, handler, m_next_layer
		);
	}

	void rekey()
	{

		assert(false);
		// m_key_exchange.reset(new key_exchange(m_host_version, m_session_id));
		// async_write(m_key_exchange->init());
	}

	virtual void async_write(opacket&& out) override
	{
		async_write(std::move(out), [this](const boost::system::error_code& ec, std::size_t)
		{
			if (ec)
				this->handle_error(ec);
		});
	}

	template<typename Handler>
	auto async_write(opacket&& p, Handler&& handler)
	{
		return async_write(m_crypto_engine.get_next_request(std::move(p)), std::move(handler));
	}

	template<typename Handler>
	auto async_write(std::unique_ptr<boost::asio::streambuf> buffer, Handler&& handler)
	{
		enum { start, writing };

		return boost::asio::async_compose<Handler, void(boost::system::error_code, std::size_t)>(
			[
				&socket = m_next_layer,
				buffer = std::move(buffer),
				conn = this->shared_from_this(),
				state = start
			]
			(auto& self, const boost::system::error_code& ec = {}, std::size_t bytes_received = 0) mutable
			{
				if (not ec and state == start)
				{
					state = writing;
					boost::asio::async_write(socket, *buffer, std::move(self));
					return;
				}

				self.complete(ec, 0);
			}, handler, m_next_layer
		);
	}

	virtual bool uses_private_key(const std::vector<uint8_t>& pk_hash) override
	{
		assert(false);
		return false;
	}

	virtual void disconnect() override
	{
		reset();

		// // copy the list since calling Close will change it
		// std::list<channel_ptr> channels(m_channels);
		// std::for_each(channels.begin(), channels.end(), [](channel_ptr c) { c->close(); });

		m_next_layer.close();
	}

  protected:

	virtual void handle_error(const boost::system::error_code &ec)
	{
		if (ec)
		{
			// std::for_each(m_channels.begin(), m_channels.end(), [&ec](channel_ptr ch) { ch->error(ec.message(), ""); });
			disconnect();

			m_auth_state = detail::auth_state_type::connected;
			// handle_connect_result(ec);
		}
	}

	template<typename Handler>
	auto async_read_packet(Handler&& handler);

  private:

	bool receive_packet(ipacket& packet, boost::system::error_code& ec);

	void received_data(const boost::system::error_code& ec);

	void process_packet(ipacket &in);
	void process_newkeys(ipacket &in, opacket &out, boost::system::error_code &ec);

	void process_channel_open(ipacket &in, opacket &out);
	void process_channel(ipacket &in, opacket &out, boost::system::error_code &ec);

	void newkeys(key_exchange& kex, boost::system::error_code &ec)
	{
		m_crypto_engine.newkeys(kex, m_auth_state == detail::auth_state_type::authenticated);
	}

	void userauth_success(const std::string& host_version, const std::vector<uint8_t>& session_id)
	{
		m_auth_state = detail::auth_state_type::authenticated;

		m_crypto_engine.enable_compression();

		m_host_version = host_version;
		m_session_id = session_id;
	}

	void async_read()
	{
		boost::asio::async_read(m_next_layer, m_response, boost::asio::transfer_at_least(1),
			[
				self = this->shared_from_this()
			]
			(const boost::system::error_code &ec, size_t bytes_transferred)
			{
				self->received_data(ec);
			});
	}

	void reset()
	{
		m_auth_state = detail::auth_state_type::none;
		m_private_key_hash.clear();
		m_session_id.clear();
		m_crypto_engine.reset();
		m_last_io = 0;
	}

	Stream m_next_layer;
	std::string m_user;

	detail::auth_state_type m_auth_state = detail::auth_state_type::none;
	std::string m_host_version;
	std::vector<uint8_t> m_session_id;

	crypto_engine m_crypto_engine;

	// connect_handler_list m_connect_handlers;

	int64_t m_last_io;
	std::vector<uint8_t> m_private_key_hash;
	boost::asio::streambuf m_response;

	validate_callback_type m_validate_host_key_cb;
	password_callback_type m_request_password_cb;
	keyboard_interactive_callback_type m_keyboard_interactive_cb;

	std::list<channel_ptr> m_channels;
	bool m_forward_agent;
	port_forward_listener *m_port_forwarder;
};

using connection = basic_connection<boost::asio::ip::tcp::socket>;

template<typename Stream>
template<typename Handler>
auto basic_connection<Stream>::async_read_packet(Handler&& handler)
{
	auto packet = std::make_unique<ipacket>();

	return boost::asio::async_compose<Handler, void(boost::system::error_code)>(
		[
			&socket = this->m_next_layer,
			conn = this->shared_from_this(),
			packet = std::move(packet),
			this
		]
		(auto& self, const boost::system::error_code& ec = {}, std::size_t bytes_transferred = 0) mutable
		{
			if (ec)
				self.complete(ec, {});
			else
				boost::asio::async_read(socket, m_response, boost::asio::transfer_at_least(1), std::move(self));
		}
	);
}


// the read loop, this routine keeps calling itself until an error condition is met
template<typename Stream>
void basic_connection<Stream>::received_data(const boost::system::error_code& ec)
{
	if (ec)
	{
		handle_error(ec);
		return;
	}

	// don't process data at all if we're no longer willing
	if (m_auth_state == detail::auth_state_type::none)
		return;

	try
	{
		auto p = m_crypto_engine.get_next_packet(m_response, ec);

		if (ec)
			handle_error(ec);
		else if (p)
			process_packet(*p);

		async_read();
	}
	catch (...)
	{
		disconnect();
	}
}

template<typename Stream>
void basic_connection<Stream>::process_packet(ipacket& in)
{
	// update time for keep alive
	struct timeval tv;
	gettimeofday(&tv, nullptr);
	m_last_io = tv.tv_sec;

	opacket out;
	boost::system::error_code ec;

	switch ((message_type)in)
	{
		case msg_disconnect:
		{
			uint32_t reasonCode;
			in >> reasonCode;
			handle_error(error::make_error_code(error::disconnect_errors(reasonCode)));
			break;
		}

		case msg_ignore:
		case msg_unimplemented:
		case msg_debug:
			break;

		case msg_service_request:
			disconnect();
			break;

		// channel
		case msg_channel_open:
			if (m_auth_state == detail::auth_state_type::authenticated)
				process_channel_open(in, out);
			break;
		case msg_channel_open_confirmation:
		case msg_channel_open_failure:
		case msg_channel_window_adjust:
		case msg_channel_data:
		case msg_channel_extended_data:
		case msg_channel_eof:
		case msg_channel_close:
		case msg_channel_request:
		case msg_channel_success:
		case msg_channel_failure:
			if (m_auth_state == detail::auth_state_type::authenticated)
				process_channel(in, out, ec);
			break;

		default:
		{
			opacket out(msg_unimplemented);
			out << m_crypto_engine.get_next_out_seq_nr();
			async_write(std::move(out));
			break;
		}
	}

	if (ec)
		// handle_connect_result(ec);
		handle_error(ec);

	if (not out.empty())
		async_write(std::move(out));
}

template<typename Stream>
bool basic_connection<Stream>::receive_packet(ipacket& packet, boost::system::error_code& ec)
{
	bool result = false;
	ec = {};

	auto p = m_crypto_engine.get_next_packet(m_response, ec);

	if (not ec and p)
	{
		result = true;
		std::swap(packet, *p);

		switch ((message_type)packet)
		{
			case msg_disconnect:
				m_next_layer.close();
				break;

			case msg_service_request:
				disconnect();
				break;

			case msg_ignore:
			case msg_unimplemented:
			case msg_debug:
				packet.clear();
				result = false;
				break;
			
			default:
				;
		}
	}

	return result;
}

template<typename Stream>
void basic_connection<Stream>::process_channel_open(ipacket &in, opacket &out)
{
	channel_ptr c;

	std::string type;

	in >> type;

	try
	{
		// if (type == "x11")
		// 	c.reset(new x11_channel(shared_from_this()));
		// else if (type == "auth-agent@openssh.com" and m_forward_agent)
		// 	c.reset(new ssh_agent_channel(this->shared_from_this()));
	}
	catch (...)
	{
	}

	if (c)
	{
		in.message(msg_channel_open_confirmation);
		c->process(in);
		m_channels.push_back(c);
	}
	else
	{
		uint32_t host_channel_id;
		in >> host_channel_id;

		const uint32_t SSH_OPEN_UNKNOWN_CHANNEL_TYPE = 3;
		out = msg_channel_open_failure;
		out << host_channel_id << SSH_OPEN_UNKNOWN_CHANNEL_TYPE << "unsupported channel type"
			<< "en";
	}
}

template<typename Stream>
void basic_connection<Stream>::process_channel(ipacket &in, opacket &out, boost::system::error_code &ec)
{
	try
	{
		uint32_t channel_id;
		in >> channel_id;

		for (channel_ptr c : m_channels)
		{
			if (c->my_channel_id() == channel_id)
			{
				c->process(in);
				break;
			}
		}
	}
	catch (...)
	{
	}
}

}

// namespace assh
// {

// class basic_connection : public std::enable_shared_from_this<basic_connection>
// {
//   protected:
// 	virtual ~basic_connection();

//   public:
// 	using executor_type = boost::asio::io_context::executor_type;

// 	basic_connection(const basic_connection &) = delete;
// 	basic_connection &operator=(const basic_connection &) = delete;

// 	// configure before connecting
// 	void set_algorithm(algorithm alg, direction dir, const std::string &preferred);

// 	// callbacks to be installed by owning object

// 	// bool validate_host_key(host, alg, key)
// 	typedef std::function<bool(const std::string &, const std::string &, const std::vector<uint8_t> &)>
// 		validate_callback_type;

// 	// void request_password()
// 	typedef std::function<void()> password_callback_type;

// 	// keyboard interactive support
// 	struct prompt
// 	{
// 		std::string str;
// 		bool echo;
// 	};
// 	typedef std::function<void(const std::string &, const std::string &, const std::vector<prompt> &)> keyboard_interactive_callback_type;

// 	virtual void set_validate_callback(const validate_callback_type &cb);
// 	void set_password_callback(const password_callback_type &cb);
// 	void set_keyboard_interactive_callback(const keyboard_interactive_callback_type &cb);

// 	template <typename Handler>
// 	void async_connect(Handler &&handler, channel_ptr opening_channel)
// 	{
// 		// BOOST_ASIO_CONNECT_HANDLER_CHECK(ConnectHandler, handler) type_check;
// 		m_connect_handlers.push_back(new connect_handler<Handler>(std::move(handler), opening_channel));
// 		start_handshake();
// 	}

// 	// to be called when requested by the connection object
// 	void password(const std::string &pw);
// 	void response(const std::vector<std::string> &responses);

// 	virtual void disconnect();
// 	virtual void rekey();

// 	void open_channel(channel_ptr ch, uint32_t id);
// 	void close_channel(channel_ptr ch, uint32_t id);

// 	bool has_open_channels();

// 	void async_write(opacket &&p)
// 	{
// 		auto self(shared_from_this());
// 		async_write(std::move(p), [self](const boost::system::error_code &ec, std::size_t) {
// 			if (ec)
// 				self->handle_error(ec);
// 		});
// 	}

// 	template <typename Handler>
// 	void async_write(opacket &&p, Handler &&handler)
// 	{
// 		async_write_packet_int(std::move(p), new write_op<Handler>(std::move(handler)));
// 	}

// 	virtual void handle_error(const boost::system::error_code &ec);

// 	void forward_agent(bool forward);
// 	void forward_port(const std::string &local_address, int16_t local_port,
// 						const std::string &remote_address, int16_t remote_port);
// 	void forward_socks5(const std::string &local_address, int16_t local_port);

// 	virtual boost::asio::io_service& get_io_service() = 0;
// 	virtual executor_type get_executor() const noexcept = 0;

// 	virtual bool is_connected() const	{ return m_authenticated; }
// 	virtual bool is_socket_open() const = 0;
// 	void keep_alive();

// 	std::string get_connection_parameters(direction d) const;
// 	std::string get_key_exchange_algoritm() const;
// 	std::vector<uint8_t>
// 	get_used_private_key() const { return m_private_key_hash; }

// 	virtual std::shared_ptr<basic_connection> get_proxy() const { return {}; }

//   protected:
// 	basic_connection(boost::asio::io_service &io_service, const std::string &user);

// 	void reset();

// 	void handle_connect_result(const boost::system::error_code &ec);

// 	struct basic_connect_handler
// 	{
// 		basic_connect_handler(channel_ptr opening_channel) : m_opening_channel(opening_channel) {}
// 		virtual ~basic_connect_handler() {}

// 		virtual void handle_connect(const boost::system::error_code &ec, boost::asio::io_service &io_service) = 0;
// 		void handle_banner(const std::string &message, const std::string &lang);

// 		//virtual void		handle_connect(const boost::system::error_code& ec) = 0;
// 		channel_ptr m_opening_channel;
// 	};

// 	typedef std::list<basic_connect_handler *> connect_handler_list;

// 	template <class Handler>
// 	struct connect_handler : public basic_connect_handler
// 	{
// 		connect_handler(Handler &&handler, channel_ptr opening_channel)
// 			: basic_connect_handler(opening_channel), m_handler(std::move(handler)) {}
// 		connect_handler(Handler &&handler, channel_ptr opening_channel, const boost::system::error_code &ec)
// 			: basic_connect_handler(opening_channel), m_handler(std::move(handler)), m_ec(ec) {}

// 		virtual void handle_connect(const boost::system::error_code &ec, boost::asio::io_service &io_service)
// 		{
// 			io_service.post(connect_handler(std::move(m_handler), std::move(m_opening_channel), ec));
// 		}

// 		void operator()()
// 		{
// 			m_handler(m_ec);
// 		}

// 		Handler m_handler;
// 		boost::system::error_code m_ec;
// 	};

// 	virtual bool validate_host_key(const std::string &pk_alg, const std::vector<uint8_t> &host_key) = 0;

// 	virtual void start_handshake();
// 	void handle_protocol_version_request(const boost::system::error_code &ec, std::size_t);
// 	void handle_protocol_version_response(const boost::system::error_code &ec, std::size_t);

// 	void received_data(const boost::system::error_code &ec);

// 	void process_packet(ipacket &in);
// 	void process_kexinit(ipacket &in, opacket &out, boost::system::error_code &ec);
// 	void process_kexdhreply(ipacket &in, opacket &out, boost::system::error_code &ec);
// 	void process_newkeys(ipacket &in, opacket &out, boost::system::error_code &ec);
// 	void process_service_accept(ipacket &in, opacket &out, boost::system::error_code &ec);
// 	void process_userauth_success(ipacket &in, opacket &out, boost::system::error_code &ec);
// 	void process_userauth_failure(ipacket &in, opacket &out, boost::system::error_code &ec);
// 	void process_userauth_banner(ipacket &in, opacket &out, boost::system::error_code &ec);
// 	void process_userauth_info_request(ipacket &in, opacket &out, boost::system::error_code &ec);

// 	void process_channel_open(ipacket &in, opacket &out);
// 	void process_channel(ipacket &in, opacket &out, boost::system::error_code &ec);

// 	template <class Handler>
// 	struct bound_handler
// 	{
// 		bound_handler(Handler handler, const boost::system::error_code &ec, ipacket &&packet)
// 			: m_handler(handler), m_ec(ec), m_packet(std::move(packet)) {}

// 		bound_handler(bound_handler &&rhs)
// 			: m_handler(std::move(rhs.m_handler)), m_ec(rhs.m_ec), m_packet(std::move(rhs.m_packet)) {}

// 		virtual void operator()() { m_handler(m_ec, m_packet); }

// 		Handler m_handler;
// 		const boost::system::error_code m_ec;
// 		ipacket m_packet;
// 	};

// 	struct basic_write_op
// 	{
// 		virtual ~basic_write_op() {}
// 		virtual void operator()(const boost::system::error_code &ec, std::size_t bytes_transferred) = 0;
// 	};

// 	template <typename Handler>
// 	struct write_op : public basic_write_op
// 	{
// 		write_op(Handler &&hander)
// 			: m_handler(std::move(hander)) {}

// 		write_op(const write_op &rhs)
// 			: m_handler(rhs.m_handler) {}

// 		write_op(write_op &&rhs)
// 			: m_handler(std::move(rhs.m_handler)) {}

// 		write_op &operator=(const write_op &rhs);

// 		virtual void operator()(const boost::system::error_code &ec, std::size_t bytes_transferred)
// 		{
// 			m_handler(ec, bytes_transferred);
// 		}

// 		Handler m_handler;
// 	};

// 	template <typename Handler>
// 	void async_write(boost::asio::streambuf *request, Handler &&handler)
// 	{
// 		async_write_int(request, new write_op<Handler>(std::move(handler)));
// 	}

// 	void async_write_packet_int(opacket &&p, basic_write_op *handler);
// 	virtual void async_write_int(boost::asio::streambuf *request, basic_write_op *handler) = 0;

// 	virtual void async_read_version_string() = 0;
// 	virtual void async_read(uint32_t at_least) = 0;

// 	void poll_channels();

// 	enum auth_state
// 	{
// 		auth_state_none,
// 		auth_state_connecting,
// 		auth_state_public_key,
// 		auth_state_keyboard_interactive,
// 		auth_state_password,
// 		auth_state_connected
// 	};

// 	boost::asio::io_service &m_io_service;

// 	std::string m_user;
// 	bool m_authenticated;
// 	connect_handler_list m_connect_handlers;
// 	std::string m_host_version;

// 	std::unique_ptr<key_exchange> m_key_exchange;

// 	std::vector<uint8_t> /*m_my_payload, m_host_payload, */m_session_id;
// 	auth_state m_auth_state;
// 	int64_t m_last_io;
// 	uint32_t m_password_attempts;
// 	std::vector<uint8_t> m_private_key_hash;
// 	uint32_t m_in_seq_nr, m_out_seq_nr;
// 	ipacket m_packet;
// 	uint32_t m_iblocksize, m_oblocksize;
// 	boost::asio::streambuf m_response;

// 	validate_callback_type m_validate_host_key_cb;
// 	password_callback_type m_request_password_cb;
// 	keyboard_interactive_callback_type m_keyboard_interactive_cb;

// 	std::unique_ptr<CryptoPP::StreamTransformation> m_decryptor;
// 	std::unique_ptr<CryptoPP::StreamTransformation> m_encryptor;
// 	std::unique_ptr<CryptoPP::MessageAuthenticationCode> m_signer;
// 	std::unique_ptr<CryptoPP::MessageAuthenticationCode> m_verifier;

// 	std::unique_ptr<compression_helper> m_compressor;
// 	std::unique_ptr<compression_helper> m_decompressor;
// 	bool m_delay_compressor, m_delay_decompressor;

// 	std::deque<opacket> m_private_keys;

// 	std::list<channel_ptr> m_channels;
// 	bool m_forward_agent;
// 	port_forward_listener *m_port_forwarder;

// 	std::string m_alg_kex,
// 		m_alg_enc_c2s, m_alg_ver_c2s, m_alg_cmp_c2s,
// 		m_alg_enc_s2c, m_alg_ver_s2c, m_alg_cmp_s2c;
// };

// // --------------------------------------------------------------------

// class connection : public basic_connection
// {
//   public:
// 	using executor_type = basic_connection::executor_type;

// 	connection(boost::asio::io_service &io_service,
// 				const std::string &user, const std::string &host, int16_t port);

// 	boost::asio::io_service& get_io_service();
	
// 	executor_type get_executor() const noexcept
// 	{
// 		// return boost::asio::get_associated_executor(m_socket);
// 		return m_io_service.get_executor();
// 	}

// 	virtual void disconnect();
// 	virtual bool is_connected() const	{ return m_socket.is_open() and basic_connection::is_connected(); }
// 	virtual bool is_socket_open() const	{ return m_socket.is_open(); }

//   protected:
// 	virtual void start_handshake();

// 	virtual bool validate_host_key(const std::string &pk_alg, const std::vector<uint8_t> &host_key);

// 	void handle_resolve(const boost::system::error_code &err, boost::asio::ip::tcp::resolver::iterator endpoint_iterator);
// 	void handle_connect(const boost::system::error_code &err, boost::asio::ip::tcp::resolver::iterator endpoint_iterator);

// 	virtual void async_write_int(boost::asio::streambuf *request, basic_write_op *op);
// 	virtual void async_read_version_string();
// 	virtual void async_read(uint32_t at_least);

//   private:
// 	boost::asio::io_service& m_io_service;
// 	boost::asio::ip::tcp::socket m_socket;
// 	boost::asio::ip::tcp::resolver m_resolver;
// 	std::string m_host;
// 	int16_t m_port;
// };

// } // namespace assh
