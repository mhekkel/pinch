//        Copyright Maarten L. Hekkelman 2013-2021
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <pinch/crypto-engine.hpp>
#include <pinch/error.hpp>
#include <pinch/pinch.hpp>

#include <boost/iostreams/filtering_stream.hpp>

#include <cryptopp/aes.h>
#include <cryptopp/cryptlib.h>
#include <cryptopp/des.h>
#include <cryptopp/factory.h>
#include <cryptopp/files.h>
#include <cryptopp/filters.h>
#include <cryptopp/gfpcrypt.h>
#include <cryptopp/modes.h>
#include <cryptopp/osrng.h>
#include <cryptopp/rsa.h>

namespace io = boost::iostreams;

// --------------------------------------------------------------------

namespace pinch
{

struct TransformDataImpl
{
	std::unique_ptr<CryptoPP::StreamTransformation> m_stream_transformation;
};

TransformData::~TransformData()
{
	delete m_impl;
}

void TransformData::clear()
{
	delete m_impl;
	m_impl = nullptr;
}

void TransformData::reset_encryptor(const std::string &name, const uint8_t *key, const uint8_t *iv)
{
	clear();

	if (name == "3des-cbc")
		m_impl = new TransformDataImpl { std::make_unique<CryptoPP::CBC_Mode<CryptoPP::DES_EDE3>::Encryption>(key, 24, iv) };
	else if (name == "aes128-cbc")
		m_impl = new TransformDataImpl { std::make_unique<CryptoPP::CBC_Mode<CryptoPP::AES>::Encryption>(key, 16, iv) };
	else if (name == "aes192-cbc")
		m_impl = new TransformDataImpl { std::make_unique<CryptoPP::CBC_Mode<CryptoPP::AES>::Encryption>(key, 24, iv) };
	else if (name == "aes256-cbc")
		m_impl = new TransformDataImpl { std::make_unique<CryptoPP::CBC_Mode<CryptoPP::AES>::Encryption>(key, 32, iv) };
	else if (name == "aes128-ctr")
		m_impl = new TransformDataImpl { std::make_unique<CryptoPP::CTR_Mode<CryptoPP::AES>::Encryption>(key, 16, iv) };
	else if (name == "aes192-ctr")
		m_impl = new TransformDataImpl { std::make_unique<CryptoPP::CTR_Mode<CryptoPP::AES>::Encryption>(key, 24, iv) };
	else if (name == "aes256-ctr")
		m_impl = new TransformDataImpl { std::make_unique<CryptoPP::CTR_Mode<CryptoPP::AES>::Encryption>(key, 32, iv) };

}

void TransformData::reset_decryptor(const std::string &name, const uint8_t *key, const uint8_t *iv)
{
	clear();

	if (name == "3des-cbc")
		m_impl = new TransformDataImpl { std::make_unique<CryptoPP::CBC_Mode<CryptoPP::DES_EDE3>::Decryption>(key, 24, iv) };
	else if (name == "aes128-cbc")
		m_impl = new TransformDataImpl { std::make_unique<CryptoPP::CBC_Mode<CryptoPP::AES>::Decryption>(key, 16, iv) };
	else if (name == "aes192-cbc")
		m_impl = new TransformDataImpl { std::make_unique<CryptoPP::CBC_Mode<CryptoPP::AES>::Decryption>(key, 24, iv) };
	else if (name == "aes256-cbc")
		m_impl = new TransformDataImpl { std::make_unique<CryptoPP::CBC_Mode<CryptoPP::AES>::Decryption>(key, 32, iv) };
	else if (name == "aes128-ctr")
		m_impl = new TransformDataImpl { std::make_unique<CryptoPP::CTR_Mode<CryptoPP::AES>::Decryption>(key, 16, iv) };
	else if (name == "aes192-ctr")
		m_impl = new TransformDataImpl { std::make_unique<CryptoPP::CTR_Mode<CryptoPP::AES>::Decryption>(key, 24, iv) };
	else if (name == "aes256-ctr")
		m_impl = new TransformDataImpl { std::make_unique<CryptoPP::CTR_Mode<CryptoPP::AES>::Decryption>(key, 32, iv) };

}

void TransformData::process(const uint8_t *in, std::size_t len, uint8_t *out)
{
	assert(m_impl);
	assert(m_impl->m_stream_transformation);
	m_impl->m_stream_transformation->ProcessData(out, in, len);
}

std::size_t TransformData::get_block_size() const
{
	return m_impl->m_stream_transformation->OptimalBlockSize();
}

// --------------------------------------------------------------------

struct MessageAuthenticationCodeImpl
{
	std::unique_ptr<CryptoPP::MessageAuthenticationCode> m_verify;
};

MessageAuthenticationCode::~MessageAuthenticationCode()
{
	delete m_impl;
}

void MessageAuthenticationCode::clear()
{
	delete m_impl;
	m_impl = nullptr;
}

void MessageAuthenticationCode::reset(const std::string &name, const uint8_t *iv)
{
	clear();

	if (name == "hmac-sha2-512")
		m_impl = new MessageAuthenticationCodeImpl{ std::make_unique<CryptoPP::HMAC<CryptoPP::SHA512>>(iv, 64) };
	else if (name == "hmac-sha2-256")
		m_impl = new MessageAuthenticationCodeImpl{ std::make_unique<CryptoPP::HMAC<CryptoPP::SHA256>>(iv, 32) };
	else if (name == "hmac-sha1")
		m_impl = new MessageAuthenticationCodeImpl{ std::make_unique<CryptoPP::HMAC<CryptoPP::SHA1>>(iv, 20) };
	else
		assert(false);
}

void MessageAuthenticationCode::update(const uint8_t *data, std::size_t len)
{
	m_impl->m_verify->Update(data, len);
}

bool MessageAuthenticationCode::verify(const uint8_t *signature)
{
	return m_impl->m_verify->Verify(signature);
}

std::size_t MessageAuthenticationCode::get_digest_size() const
{
	return m_impl->m_verify->DigestSize();
}

// --------------------------------------------------------------------

struct packet_encryptor
{
	typedef char char_type;
	struct category : io::multichar_output_filter_tag, io::flushable_tag
	{
	};

	packet_encryptor(CryptoPP::StreamTransformation &cipher,
	                 CryptoPP::MessageAuthenticationCode &signer, uint32_t blocksize, uint32_t seq_nr)
		: m_cipher(cipher)
		, m_signer(signer)
		, m_blocksize(blocksize)
		, m_flushed(false)
	{
		for (int i = 3; i >= 0; --i)
		{
			uint8_t ch = static_cast<uint8_t>(seq_nr >> (i * 8));
			m_signer.Update(&ch, 1);
		}

		m_block.reserve(m_blocksize);
	}

	template <typename Sink>
	std::streamsize write(Sink &sink, const char *s, std::streamsize n)
	{
		std::streamsize result = 0;

		for (std::streamsize o = 0; o < n; o += m_blocksize)
		{
			size_t k = n;
			if (k > m_blocksize - m_block.size())
				k = m_blocksize - m_block.size();

			const uint8_t *sp = reinterpret_cast<const uint8_t *>(s);

			m_signer.Update(sp, static_cast<size_t>(k));
			m_block.insert(m_block.end(), sp, sp + k);

			result += k;
			s += k;

			if (m_block.size() == m_blocksize)
			{
				blob block(m_blocksize);
				m_cipher.ProcessData(block.data(), m_block.data(), m_blocksize);

				for (uint32_t i = 0; i < m_blocksize; ++i)
					io::put(sink, block[i]);

				m_block.clear();
			}
		}

		return result;
	}

	template <typename Sink>
	bool flush(Sink &sink)
	{
		if (not m_flushed)
		{
			assert(m_block.size() == 0);

			blob digest(m_signer.DigestSize());
			m_signer.Final(digest.data());
			for (size_t i = 0; i < digest.size(); ++i)
				io::put(sink, digest[i]);

			m_flushed = true;
		}

		return true;
	}

	CryptoPP::StreamTransformation &m_cipher;
	CryptoPP::MessageAuthenticationCode &m_signer;
	blob m_block;
	uint32_t m_blocksize;
	bool m_flushed;
};

// --------------------------------------------------------------------

crypto_engine::crypto_engine()
{
}

void crypto_engine::newkeys(key_exchange &kex, bool authenticated)
{
	using namespace CryptoPP;

	// Client to server encryption
	m_alg_enc_c2s = kex.get_encryption_protocol(direction::c2s);

	const uint8_t *key = kex.key(key_exchange::C);
	const uint8_t *iv = kex.key(key_exchange::A);

	m_encryptor.reset_encryptor(m_alg_enc_c2s, key, iv);

	// Server to client encryption
	m_alg_enc_s2c = kex.get_encryption_protocol(direction::s2c);

	key = kex.key(key_exchange::D);
	iv = kex.key(key_exchange::B);

	m_decryptor.reset_decryptor(m_alg_enc_s2c, key, iv);

	// Client To Server verification
	m_alg_ver_c2s = kex.get_verification_protocol(direction::c2s);
	iv = kex.key(key_exchange::E);

	m_signer.reset(m_alg_ver_c2s, iv);

	// Server to Client verification

	m_alg_ver_s2c = kex.get_verification_protocol(direction::s2c);
	iv = kex.key(key_exchange::F);

	m_verifier.reset(m_alg_ver_s2c, iv);

	// Client to Server compression
	m_alg_cmp_c2s = kex.get_compression_protocol(direction::c2s);
	if ((not m_compressor and m_alg_cmp_c2s == "zlib") or (authenticated and m_alg_cmp_c2s == "zlib@openssh.com"))
		m_compressor.reset(new compression_helper(true));
	else if (m_alg_cmp_c2s == "zlib@openssh.com")
		m_delay_compressor = true;

	// Server to Client compression
	m_alg_cmp_s2c = kex.get_compression_protocol(direction::s2c);
	if ((not m_decompressor and m_alg_cmp_s2c == "zlib") or (authenticated and m_alg_cmp_s2c == "zlib@openssh.com"))
		m_decompressor.reset(new compression_helper(false));
	else if (m_alg_cmp_c2s == "zlib@openssh.com")
		m_delay_decompressor = true;

	if (m_decryptor)
	{
		m_iblocksize = m_decryptor.get_block_size();
		m_oblocksize = m_encryptor.get_block_size();
	}
}

void crypto_engine::reset()
{
	m_packet.reset();
	m_encryptor.clear();
	m_decryptor.clear();
	m_signer.clear();
	m_verifier.clear();
	m_compressor.reset(nullptr);
	m_decompressor.reset(nullptr);
	m_delay_decompressor = m_delay_compressor = false;
	m_in_seq_nr = m_out_seq_nr = 0;
	m_iblocksize = m_oblocksize = 8;

	m_alg_kex.clear();
	m_alg_enc_c2s.clear();
	m_alg_ver_c2s.clear();
	m_alg_cmp_c2s.clear();
	m_alg_enc_s2c.clear();
	m_alg_ver_s2c.clear();
	m_alg_cmp_s2c.clear();
}

std::string crypto_engine::get_connection_parameters(direction dir) const
{
	std::string result;

	if (dir == direction::c2s)
	{
		result = m_alg_enc_c2s + '/' + m_alg_ver_c2s;

		if (m_alg_cmp_c2s != "none")
			result = result + '/' + m_alg_cmp_c2s;
	}
	else
	{
		result = m_alg_enc_s2c + '/' + m_alg_ver_s2c;

		if (m_alg_cmp_s2c != "none")
			result = result + '/' + m_alg_cmp_s2c;
	}

	return result;
}

std::string crypto_engine::get_key_exchange_algorithm() const
{
	return m_alg_kex;
}

blob crypto_engine::get_next_block(boost::asio::streambuf &buffer, bool empty)
{
	blob block(m_iblocksize);
	buffer.sgetn(reinterpret_cast<char *>(block.data()), m_iblocksize);

	if (m_decryptor)
	{
		blob data(m_iblocksize);
		m_decryptor.process(block.data(), m_iblocksize, data.data());
		std::swap(data, block);
	}

	if (m_verifier)
	{
		if (empty)
		{
			for (int32_t i = 3; i >= 0; --i)
			{
				uint8_t b = m_in_seq_nr >> (i * 8);
				m_verifier.update(&b, 1);
			}
		}

		m_verifier.update(block.data(), block.size());
	}

	return block;
}

std::unique_ptr<ipacket> crypto_engine::get_next_packet(boost::asio::streambuf &buffer, boost::system::error_code &ec)
{
	std::lock_guard lock(m_in_mutex);

	if (not m_packet)
		m_packet = std::make_unique<ipacket>(m_in_seq_nr);

	bool complete_and_verified = false;

	while (buffer.size() >= m_iblocksize)
	{
		if (not m_packet->complete())
			m_packet->append(get_next_block(buffer, m_packet->empty()));

		if (m_packet->complete())
		{
			if (m_verifier)
			{
				const std::size_t digest_size = m_verifier.get_digest_size();

				if (buffer.size() < digest_size)
					break;

				blob digest(digest_size);
				buffer.sgetn(reinterpret_cast<char *>(digest.data()), digest_size);

				if (not m_verifier.verify(digest.data()))
				{
					ec = error::make_error_code(error::mac_error);
					break;
				}
			}

			if (m_decompressor)
				m_packet->decompress(*m_decompressor, ec);

			++m_in_seq_nr;

			complete_and_verified = true;
			break;
		}
	}

	return complete_and_verified ? std::move(m_packet) : std::unique_ptr<ipacket>();
}

std::unique_ptr<boost::asio::streambuf> crypto_engine::get_next_request(opacket &&p)
{
	std::lock_guard lock(m_out_mutex);

	auto request = std::make_unique<boost::asio::streambuf>();

	if (m_compressor)
	{
		boost::system::error_code ec;
		p.compress(*m_compressor, ec);

		if (ec)
			throw ec;
	}

	io::filtering_stream<io::output> out;
	if (m_encryptor)
		out.push(packet_encryptor(*m_encryptor.m_impl->m_stream_transformation, *m_signer.m_impl->m_verify, m_oblocksize, m_out_seq_nr));
	out.push(*request);

	p.write(out, m_oblocksize);

	++m_out_seq_nr;

	return request;
}

void crypto_engine::enable_compression()
{
	if (m_delay_compressor)
		m_compressor.reset(new compression_helper(true));

	if (m_delay_decompressor)
		m_decompressor.reset(new compression_helper(false));
}

} // namespace pinch
