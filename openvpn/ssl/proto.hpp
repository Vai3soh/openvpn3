//    OpenVPN -- An application to securely tunnel IP networks
//               over a single port, with support for SSL/TLS-based
//               session authentication and key exchange,
//               packet encryption, packet authentication, and
//               packet compression.
//
//    Copyright (C) 2012-2015 OpenVPN Technologies, Inc.
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU Affero General Public License Version 3
//    as published by the Free Software Foundation.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU Affero General Public License for more details.
//
//    You should have received a copy of the GNU Affero General Public License
//    along with this program in the COPYING file.
//    If not, see <http://www.gnu.org/licenses/>.

// ProtoContext, the fundamental OpenVPN protocol implementation.
// It can be used by OpenVPN clients, servers, or unit tests.

#ifndef OPENVPN_SSL_PROTO_H
#define OPENVPN_SSL_PROTO_H

#include <cstring>
#include <string>
#include <sstream>
#include <algorithm>                  // for std::min

#include <boost/cstdint.hpp>          // for boost::uint32_t, etc.
#include <boost/algorithm/string.hpp> // for boost::algorithm::starts_with

#include <openvpn/common/exception.hpp>
#include <openvpn/common/types.hpp>
#include <openvpn/common/version.hpp>
#include <openvpn/common/platform_name.hpp>
#include <openvpn/common/rc.hpp>
#include <openvpn/common/hexstr.hpp>
#include <openvpn/common/options.hpp>
#include <openvpn/common/mode.hpp>
#include <openvpn/common/socktypes.hpp>
#include <openvpn/common/number.hpp>
#include <openvpn/common/scoped_ptr.hpp>
#include <openvpn/common/likely.hpp>
#include <openvpn/buffer/buffer.hpp>
#include <openvpn/buffer/safestr.hpp>
#include <openvpn/time/time.hpp>
#include <openvpn/time/durhelper.hpp>
#include <openvpn/frame/frame.hpp>
#include <openvpn/random/randapi.hpp>
#include <openvpn/crypto/cryptoalgs.hpp>
#include <openvpn/crypto/cryptodc.hpp>
#include <openvpn/crypto/cipher.hpp>
#include <openvpn/crypto/ovpnhmac.hpp>
#include <openvpn/crypto/packet_id.hpp>
#include <openvpn/crypto/static_key.hpp>
#include <openvpn/log/sessionstats.hpp>
#include <openvpn/ssl/protostack.hpp>
#include <openvpn/ssl/psid.hpp>
#include <openvpn/ssl/tlsprf.hpp>
#include <openvpn/transport/protocol.hpp>
#include <openvpn/tun/layer.hpp>
#include <openvpn/compress/compress.hpp>
#include <openvpn/ssl/proto_context_options.hpp>

#if OPENVPN_DEBUG_PROTO >= 1
#define OPENVPN_LOG_PROTO(x) OPENVPN_LOG(x)
#else
#define OPENVPN_LOG_PROTO(x)
#endif

#if OPENVPN_DEBUG_PROTO >= 2
#define OPENVPN_LOG_PROTO_VERBOSE(x) OPENVPN_LOG(x)
#else
#define OPENVPN_LOG_PROTO_VERBOSE(x)
#endif

/*

ProtoContext -- OpenVPN protocol implementation

Protocol negotiation states:

Client:

1. send client reset to server
2. wait for server reset from server AND ack from 1 (C_WAIT_RESET, C_WAIT_RESET_ACK)
3. start SSL handshake
4. send auth message to server
5. wait for server auth message AND ack from 4 (C_WAIT_AUTH, C_WAIT_AUTH_ACK)
6. go active (ACTIVE)

Server:

1. wait for client reset (S_WAIT_RESET)
2. send server reset to client
3. wait for ACK from 2 (S_WAIT_RESET_ACK)
4. start SSL handshake
5. wait for auth message from client (S_WAIT_AUTH)
6. send auth message to client
7. wait for ACK from 6 (S_WAIT_AUTH_ACK)
8. go active (ACTIVE)

*/

namespace openvpn {

  // utility namespace for ProtoContext
  namespace proto_context_private {
    static const unsigned char auth_prefix[] = { 0, 0, 0, 0, 2 }; // CONST GLOBAL

    static const unsigned char keepalive_message[] = {    // CONST GLOBAL
      0x2a, 0x18, 0x7b, 0xf3, 0x64, 0x1e, 0xb4, 0xcb,
      0x07, 0xed, 0x2d, 0x0a, 0x98, 0x1f, 0xc7, 0x48
    };

    enum {
      KEEPALIVE_FIRST_BYTE = 0x2a  // first byte of keepalive message
    };

    inline bool is_keepalive(const Buffer& buf)
    {
      return buf.size() >= sizeof(keepalive_message)
	&& buf[0] == KEEPALIVE_FIRST_BYTE
	&& !std::memcmp(keepalive_message, buf.c_data(), sizeof(keepalive_message));
    }

    static const unsigned char explicit_exit_notify_message[] = {    // CONST GLOBAL
      0x28, 0x7f, 0x34, 0x6b, 0xd4, 0xef, 0x7a, 0x81,
      0x2d, 0x56, 0xb8, 0xd3, 0xaf, 0xc5, 0x45, 0x9c,
      6 // OCC_EXIT
    };

    enum {
      EXPLICIT_EXIT_NOTIFY_FIRST_BYTE = 0x28  // first byte of exit message
    };
  }

  class ProtoContext
  {
  protected:
    enum {
      // packet opcode (high 5 bits) and key-id (low 3 bits) are combined in one byte
      KEY_ID_MASK =             0x07,
      OPCODE_SHIFT =            3,

      // packet opcodes -- the V1 is intended to allow protocol changes in the future
      //CONTROL_HARD_RESET_CLIENT_V1 = 1,   // (obsolete) initial key from client, forget previous state
      //CONTROL_HARD_RESET_SERVER_V1 = 2,   // (obsolete) initial key from server, forget previous state
      CONTROL_SOFT_RESET_V1 =        3,   // new key, graceful transition from old to new key
      CONTROL_V1 =                   4,   // control channel packet (usually TLS ciphertext)
      ACK_V1 =                       5,   // acknowledgement for packets received
      DATA_V1 =                      6,   // data channel packet with 1-byte header
      DATA_V2 =                      9,   // data channel packet with 4-byte header

      // indicates key_method >= 2
      CONTROL_HARD_RESET_CLIENT_V2 = 7,   // initial key from client, forget previous state
      CONTROL_HARD_RESET_SERVER_V2 = 8,   // initial key from server, forget previous state

      // define the range of legal opcodes
      FIRST_OPCODE =                3,
      LAST_OPCODE =                 9,
      INVALID_OPCODE =              0,

      // DATA_V2 constants
      OP_SIZE_V2 = 4,                // size of initial packet opcode
      OP_PEER_ID_UNDEF = 0x00FFFFFF, // indicates that Peer ID is undefined

      // states
      // C_x : client states
      // S_x : server states

      // ACK states -- must be first before other states
      STATE_UNDEF=-1,
      C_WAIT_RESET_ACK=0,
      C_WAIT_AUTH_ACK=1,
      S_WAIT_RESET_ACK=2,
      S_WAIT_AUTH_ACK=3,
      LAST_ACK_STATE=3, // all ACK states must be <= this value

      // key negotiation states (client)
      C_INITIAL=4,
      C_WAIT_RESET=5,   // must be C_INITIAL+1
      C_WAIT_AUTH=6,

      // key negotiation states (server)
      S_INITIAL=7,
      S_WAIT_RESET=8,   // must be S_INITIAL+1
      S_WAIT_AUTH=9,

      // key negotiation states (client and server)
      ACTIVE=10,
    };

    static unsigned int opcode_extract(const unsigned int op)
    {
      return op >> OPCODE_SHIFT;
    }

    static unsigned int key_id_extract(const unsigned int op)
    {
      return op & KEY_ID_MASK;
    }

    static size_t op_head_size(const unsigned int op)
    {
      return opcode_extract(op) == DATA_V2 ? OP_SIZE_V2 : 1;
    }

    static unsigned int op_compose(const unsigned int opcode, const unsigned int key_id)
    {
      return (opcode << OPCODE_SHIFT) | key_id;
    }

    static unsigned int op32_compose(const unsigned int opcode,
				     const unsigned int key_id,
				     const int op_peer_id)
    {
      return (op_compose(opcode, key_id) << 24) | (op_peer_id & 0x00FFFFFF);
    }

  public:
    OPENVPN_SIMPLE_EXCEPTION(peer_psid_undef);
    OPENVPN_SIMPLE_EXCEPTION(bad_auth_prefix);
    OPENVPN_EXCEPTION(process_server_push_error);
    OPENVPN_EXCEPTION_INHERIT(option_error, proto_option_error);

    // configuration data passed to ProtoContext constructor
    class Config : public RCCopyable<thread_unsafe_refcount>
    {
    public:
      typedef boost::intrusive_ptr<Config> Ptr;

      Config()
      {
	reliable_window = 0;
	max_ack_list = 0;
	pid_mode = 0;
	pid_seq_backtrack = 0;
	pid_time_backtrack = 0;
	xmit_creds = true;
	key_direction = -1; // bidirectional
	dc_deferred = false;
	enable_op32 = false;
	remote_peer_id = -1;
	local_peer_id = -1;
	tun_mtu = 1500;
	force_aes_cbc_ciphersuites = false;
      }

      // master SSL context factory
      SSLFactoryAPI::Ptr ssl_factory;

      // data channel
      CryptoDCSettings dc;

      // TLSPRF factory
      TLSPRFFactory::Ptr tlsprf_factory;

      // master Frame object
      Frame::Ptr frame;

      // (non-smart) pointer to current time
      TimePtr now;

      // Random number generator.
      // Use-cases demand highest cryptographic strength
      // such as key generation.
      RandomAPI::Ptr rng;

      // Pseudo-random number generator.
      // Use-cases demand cryptographic strength
      // combined with high performance.  Used for
      // IV and ProtoSessionID generation.
      RandomAPI::Ptr prng;

      // defer data channel initialization until after client options pull
      bool dc_deferred;

      // transmit username/password creds to server (client-only)
      bool xmit_creds;

      // Transport protocol, i.e. UDPv4, etc.
      Protocol protocol; // set with set_protocol()

      // OSI layer
      Layer layer;

      // compressor
      CompressContext comp_ctx;

      // tls_auth parms
      OpenVPNStaticKey tls_auth_key; // leave this undefined to disable tls_auth
      OvpnHMACFactory::Ptr tls_auth_factory;
      OvpnHMACContext::Ptr tls_auth_context;
      int key_direction; // 0, 1, or -1 for bidirectional

      // reliability layer parms
      reliable::id_t reliable_window;
      size_t max_ack_list;

      // packet_id parms for both data and control channels
      int pid_mode;            // PacketIDReceive::UDP_MODE or PacketIDReceive::TCP_MODE
      int pid_seq_backtrack;
      int pid_time_backtrack;

      // timeout parameters, relative to construction of KeyContext object
      Time::Duration handshake_window; // SSL/TLS negotiation must complete by this time
      Time::Duration become_primary;   // KeyContext (that is ACTIVE) becomes primary at this time
      Time::Duration renegotiate;      // start SSL/TLS renegotiation at this time
      Time::Duration expire;           // KeyContext expires at this time

      // keepalive parameters
      Time::Duration keepalive_ping;
      Time::Duration keepalive_timeout;

      // GUI version, passed to server as IV_GUI_VER
      std::string gui_version;

      // op header
      bool enable_op32;
      int remote_peer_id; // -1 to disable
      int local_peer_id;  // -1 to disable

      // MTU
      unsigned int tun_mtu;

      // Compatibility
      bool force_aes_cbc_ciphersuites;

      void load(const OptionList& opt, const ProtoContextOptions& pco,
		const int default_key_direction, const bool server)
      {
	// first set defaults
	reliable_window = 4;
	max_ack_list = 4;
	pid_seq_backtrack = 64;
	pid_time_backtrack = 30;
	handshake_window = Time::Duration::seconds(60);
	renegotiate = Time::Duration::seconds(3600);
	keepalive_ping = Time::Duration::seconds(8);
	keepalive_timeout = Time::Duration::seconds(40);
	comp_ctx = CompressContext(CompressContext::NONE, false);
	protocol = Protocol();
	pid_mode = PacketIDReceive::UDP_MODE;
	key_direction = default_key_direction;

	// load parameters that can be present in both config file or pushed options
	load_common(opt, pco, server);

	// layer
	{
	  const Option* dev = opt.get_ptr("dev-type");
	  if (!dev)
	    dev = opt.get_ptr("dev");
	  if (!dev)
	    throw proto_option_error("missing dev-type or dev option");
	  const std::string& dev_type = dev->get(1, 64);
	  if (boost::algorithm::starts_with(dev_type, "tun"))
	    layer = Layer(Layer::OSI_LAYER_3);
	  else if (boost::algorithm::starts_with(dev_type, "tap"))
	    layer = Layer(Layer::OSI_LAYER_2);
	  else
	    throw proto_option_error("bad dev-type");
	}

	// cipher/digest/tls-auth
	{
	  CryptoAlgs::Type cipher = CryptoAlgs::NONE;
	  CryptoAlgs::Type digest = CryptoAlgs::NONE;

	  // data channel cipher
	  {
	    const Option *o = opt.get_ptr("cipher");
	    if (o)
	      {
		const std::string& cipher_name = o->get(1, 128);
		if (cipher_name != "none")
		  cipher = CryptoAlgs::lookup(cipher_name);
	      }
	    else
	      cipher = CryptoAlgs::lookup("BF-CBC");
	  }

	  // data channel HMAC
	  {
	    const Option *o = opt.get_ptr("auth");
	    if (o)
	      {
		const std::string& auth_name = o->get(1, 128);
		if (auth_name != "none")
		  digest = CryptoAlgs::lookup(auth_name);
	      }
	    else
	      digest = CryptoAlgs::lookup("SHA1");
	  }
	  dc.set_cipher(cipher);
	  dc.set_digest(digest);

	  // tls-auth
	  {
	    const Option *o = opt.get_ptr("tls-auth");
	    if (o)
	      {
		tls_auth_key.parse(o->get(1, 0));

		const Option *tad = opt.get_ptr("tls-auth-digest");
		if (tad)
		  digest = CryptoAlgs::lookup(tad->get(1, 128));
		set_tls_auth_digest(digest);
	      }
	  }
	}

	// key-direction
	{
	  if (key_direction >= -1 && key_direction <= 1)
	    {
	      const Option *o = opt.get_ptr("key-direction");
	      if (o)
		{
		  const std::string& dir = o->get(1, 16);
		  if (dir == "0")
		    key_direction = 0;
		  else if (dir == "1")
		    key_direction = 1;
		  else if (dir == "bidirectional" || dir == "bi")
		    key_direction = -1;
		  else
		    throw proto_option_error("bad key-direction parameter");
		}
	    }
	  else
	    throw proto_option_error("bad key-direction default");
	}

	// compression
	{
	  const Option *o = opt.get_ptr("compress");
	  if (o)
	    {
	      if (o->size() >= 2)
		{
		  const std::string meth_name = o->get(1, 128);
		  CompressContext::Type meth = CompressContext::parse_method(meth_name);
		  if (meth == CompressContext::NONE)
		    OPENVPN_THROW(proto_option_error, "Unknown compressor: '" << meth_name << '\'');
		  comp_ctx = CompressContext(pco.is_comp() ? meth : CompressContext::stub(meth), pco.is_comp_asym());
		}
	      else
		comp_ctx = CompressContext(pco.is_comp() ? CompressContext::ANY : CompressContext::COMP_STUB, pco.is_comp_asym());
	    }
	  else
	    {
	      o = opt.get_ptr("comp-lzo");
	      if (o)
		{
		  if (o->size() == 2 && o->ref(1) == "no")
		    {
		      // On the client, by using ANY instead of ANY_LZO, we are telling the server
		      // that it's okay to use any of our supported compression methods.
		      comp_ctx = CompressContext(pco.is_comp() ? CompressContext::ANY : CompressContext::LZO_STUB, pco.is_comp_asym());
		    }
		  else
		    {
		      comp_ctx = CompressContext(pco.is_comp() ? CompressContext::LZO : CompressContext::LZO_STUB, pco.is_comp_asym());
		    }
		}
	    }
	}

	// tun-mtu
	try {
	  const Option *o = opt.get_ptr("tun-mtu");
	  if (o)
	    {
	      bool status = parse_number_validate<unsigned int>(o->get(1, 16),
								16,
								576,
								65535,
								&tun_mtu);
	      if (!status)
		throw Exception("parse/range issue");
	    }
	}
	catch (const std::exception& e)
	  {
	    OPENVPN_THROW(proto_option_error, "tun-mtu directive: " << e.what());
	  }
      }

      // load options string pushed by server
      void process_push(const OptionList& opt, const ProtoContextOptions& pco)
      {
	try {
	  // load parameters that can be present in both config file or pushed options
	  load_common(opt, pco, false);
	}
	catch (const std::exception& e)
	  {
	    OPENVPN_THROW(process_server_push_error, "Problem accepting server-pushed parameter: " << e.what());
	  }

	// data channel
	{
	  // cipher
	  std::string new_cipher;
	  try {
	    const Option *o = opt.get_ptr("cipher");
	    if (o)
	      {
		new_cipher = o->get(1, 128);
		if (new_cipher != "none")
		  dc.set_cipher(CryptoAlgs::lookup(new_cipher));
	      }
	  }
	  catch (const std::exception& e)
	    {
	      OPENVPN_THROW(process_server_push_error, "Problem accepting server-pushed cipher '" << new_cipher << "': " << e.what());
	    }

	  // digest
	  std::string new_digest;
	  try {
	    const Option *o = opt.get_ptr("auth");
	    if (o)
	      {
		new_digest = o->get(1, 128);
		if (new_digest != "none")
		  dc.set_digest(CryptoAlgs::lookup(new_digest));
	      }
	  }
	  catch (const std::exception& e)
	    {
	      OPENVPN_THROW(process_server_push_error, "Problem accepting server-pushed digest '" << new_digest << "': " << e.what());
	    }
	}

	// compression
	std::string new_comp;
	try {
	  const Option *o;
	  o = opt.get_ptr("compress");
	  if (o)
	    {
	      new_comp = o->get(1, 128);
	      CompressContext::Type meth = CompressContext::parse_method(new_comp);
	      if (meth != CompressContext::NONE)
		comp_ctx = CompressContext(pco.is_comp() ? meth : CompressContext::stub(meth), pco.is_comp_asym());
	    }
	  else
	    {
	      o = opt.get_ptr("comp-lzo");
	      if (o)
		{
		  if (o->size() == 2 && o->ref(1) == "no")
		    {
		      comp_ctx = CompressContext(CompressContext::LZO_STUB, false);
		    }
		  else
		    {
		      comp_ctx = CompressContext(pco.is_comp() ? CompressContext::LZO : CompressContext::LZO_STUB, pco.is_comp_asym());
		    }
		}
	    }
	}
	catch (const std::exception& e)
	  {
	    OPENVPN_THROW(process_server_push_error, "Problem accepting server-pushed compressor '" << new_comp << "': " << e.what());
	  }

	// peer ID
	try {
	  const Option *o = opt.get_ptr("peer-id");
	  if (o)
	    {
	      bool status = parse_number_validate<int>(o->get(1, 16),
						       16,
						       -1,
						       0xFFFFFE,
						       &remote_peer_id);
	      if (!status)
		throw Exception("parse/range issue");
	      enable_op32 = true;
	    }
	}
	catch (const std::exception& e)
	  {
	    OPENVPN_THROW(process_server_push_error, "Problem accepting server-pushed peer-id: " << e.what());
	  }
      }

      void set_pid_mode(const bool tcp_linear)
      {
	if (protocol.is_udp() || !tcp_linear)
	  pid_mode = PacketIDReceive::UDP_MODE;
	else if (protocol.is_tcp())
	  pid_mode = PacketIDReceive::TCP_MODE;
	else
	  throw proto_option_error("transport protocol undefined");
      }

      void set_protocol(const Protocol& p)
      {
	// adjust options for new transport protocol
	protocol = p;
	set_pid_mode(false);
      }

      void set_tls_auth_digest(const CryptoAlgs::Type digest)
      {
	tls_auth_context = tls_auth_factory->new_obj(digest);
      }

      void set_xmit_creds(const bool xmit_creds_arg)
      {
	xmit_creds = xmit_creds_arg;
      }

      bool tls_auth_enabled() const
      {
	return tls_auth_key.defined() && tls_auth_context;
      }

      void validate_complete() const
      {
	if (!protocol.defined())
	  throw proto_option_error("transport protocol undefined");
      }

      // generate a string summarizing options that will be
      // transmitted to peer for options consistency check
      std::string options_string()
      {
	std::ostringstream out;

	const bool server = ssl_factory->mode().is_server();

	out << "V4";

	out << ",dev-type " << layer.dev_type();
	out << ",link-mtu " << tun_mtu + link_mtu_adjust();
	out << ",tun-mtu " << tun_mtu;
	out << ",proto " << protocol.str_client(true);
	
	{
	  const char *compstr = comp_ctx.options_string();
	  if (compstr)
	    out << ',' << compstr;
	}

	if (key_direction >= 0)
	  out << ",keydir " << key_direction;

	out << ",cipher " << CryptoAlgs::name(dc.cipher(), "[null-cipher]");
	out << ",auth " << CryptoAlgs::name(dc.digest(), "[null-digest]");
	out << ",keysize " << (CryptoAlgs::key_length(dc.cipher()) * 8);

	if (tls_auth_key.defined())
	  out << ",tls-auth";
	out << ",key-method 2";

	if (server)
	  out << ",tls-server";
	else
	  out << ",tls-client";

	return out.str();
      }

      // generate a string summarizing information about the client
      // including capabilities
      std::string peer_info_string() const
      {
	std::ostringstream out;
	const char *compstr = NULL;

	if (!gui_version.empty())
	  out << "IV_GUI_VER=" << gui_version << '\n';
	out << "IV_VER=" << OPENVPN_VERSION << '\n';
	out << "IV_PLAT=" << platform_name() << '\n';
	if (!force_aes_cbc_ciphersuites)
	  {
	    out << "IV_NCP=2\n"; // negotiable crypto parameters V2
	    out << "IV_TCPNL=1\n"; // supports TCP non-linear packet ID
	    out << "IV_PROTO=2\n"; // supports op32 and P_DATA_V2
	    compstr = comp_ctx.peer_info_string();
	  }
	else
	  compstr = comp_ctx.peer_info_string_v1();
	if (compstr)
	  out << compstr;
	const std::string ret = out.str();
	OPENVPN_LOG_PROTO("Peer Info:" << std::endl << ret);
	return ret;
      }

      // Used to generate link_mtu option sent to peer.
      // Not const because dc.context() caches the DC context.
      unsigned int link_mtu_adjust()
      {
	const size_t adj = protocol.extra_transport_bytes() + // extra 2 bytes for TCP-streamed packet length
          (enable_op32 ? 4 : 1) +                        // leading op
	  comp_ctx.extra_payload_bytes() +               // compression header
	  PacketID::size(PacketID::SHORT_FORM) +         // sequence number
	  dc.context().encap_overhead();                 // data channel crypto layer overhead
	return (unsigned int)adj;
      }

    private:
      // load parameters that can be present in both config file or pushed options
      void load_common(const OptionList& opt, const ProtoContextOptions& pco,
		       const bool server)
      {
	// duration parms
	load_duration_parm(renegotiate, "reneg-sec", opt, 10, false);
	expire = renegotiate;
	load_duration_parm(expire, "tran-window", opt, 10, false);
	expire += renegotiate;
	load_duration_parm(handshake_window, "hand-window", opt, 10, false);
	become_primary = Time::Duration::seconds(std::min(handshake_window.to_seconds(),
							    renegotiate.to_seconds() / 2));
	// keepalive, ping, ping-restart
	{
	  const Option *o = opt.get_ptr("keepalive");
	  if (o)
	    {
	      set_duration_parm(keepalive_ping, "keepalive ping", o->get(1, 16), 1, false);
	      set_duration_parm(keepalive_timeout, "keepalive timeout", o->get(2, 16), 1, server);
	    }
	  else
	    {
	      load_duration_parm(keepalive_ping, "ping", opt, 1, false);
	      load_duration_parm(keepalive_timeout, "ping-restart", opt, 1, false);
	    }
	}
      }
    };

    // Used to describe an incoming network packet
    class PacketType
    {
      friend class ProtoContext;

      enum {
	DEFINED=1<<0,     // packet is valid (otherwise invalid)
	CONTROL=1<<1,     // packet for control channel (otherwise for data channel)
	SECONDARY=1<<2,   // packet is associated with secondary KeyContext (otherwise primary)
	SOFT_RESET=1<<3,  // packet is a CONTROL_SOFT_RESET_V1 msg indicating a request for SSL/TLS renegotiate
      };

    public:
      bool is_defined() const { return flags & DEFINED; }
      bool is_control() const { return (flags & (CONTROL|DEFINED)) == (CONTROL|DEFINED); }
      bool is_data()    const { return (flags & (CONTROL|DEFINED)) == DEFINED; }
      bool is_soft_reset() const { return (flags & (CONTROL|DEFINED|SECONDARY|SOFT_RESET))
	                                        == (CONTROL|DEFINED|SECONDARY|SOFT_RESET); }
      int peer_id() const { return peer_id_; }

    private:
      PacketType(const Buffer& buf, class ProtoContext& proto)
	: flags(0), opcode(INVALID_OPCODE), peer_id_(-1)
      {
	if (likely(buf.size()))
	  {
	    // get packet header byte
	    const unsigned int op = buf[0];

	    // examine opcode
	    {
	      const unsigned int opc = opcode_extract(op);
	      switch (opc)
		{
		case CONTROL_SOFT_RESET_V1:
		case CONTROL_V1:
		case ACK_V1:
		  {
		    flags |= CONTROL;
		    opcode = opc;
		    break;
		  }
		case DATA_V2:
		  {
		    if (unlikely(buf.size() < 4))
		      return;
		    const int opi = ntohl(*(const boost::uint32_t *)buf.c_data()) & 0x00FFFFFF;
		    if (opi != OP_PEER_ID_UNDEF)
		      peer_id_ = opi;
		    opcode = opc;
		    break;
		  }
		case DATA_V1:
		  {
		    opcode = opc;
		    break;
		  }
		case CONTROL_HARD_RESET_CLIENT_V2:
		  {
		    if (!proto.is_server())
		      return;
		    flags |= CONTROL;
		    opcode = opc;
		    break;
		  }
		case CONTROL_HARD_RESET_SERVER_V2:
		  {
		    if (proto.is_server())
		      return;
		    flags |= CONTROL;
		    opcode = opc;
		    break;
		  }
		default:
		  return;
		}
	    }

	    // examine key ID
	    {
	      const unsigned int kid = key_id_extract(op);
	      if (kid == proto.primary->key_id())
		flags |= DEFINED;
	      else if (proto.secondary && kid == proto.secondary->key_id())
		flags |= (DEFINED | SECONDARY);
	      else if (opcode == CONTROL_SOFT_RESET_V1 && kid == proto.upcoming_key_id)
		flags |= (DEFINED | SECONDARY | SOFT_RESET);
	    }
	  }
      }

      bool is_secondary() const { return flags & SECONDARY; }

      unsigned int flags;
      unsigned int opcode;
      int peer_id_;
    };

#ifdef OPENVPN_INSTRUMENTATION
    static const char *opcode_name(const unsigned int opcode)
    {
      switch (opcode)
	{
	case CONTROL_SOFT_RESET_V1:
	  return "CONTROL_SOFT_RESET_V1";
	case CONTROL_V1:
	  return "CONTROL_V1";
	case ACK_V1:
	  return "ACK_V1";
	case DATA_V1:
	  return "DATA_V1";
	case DATA_V2:
	  return "DATA_V2";
	case CONTROL_HARD_RESET_CLIENT_V2:
	  return "CONTROL_HARD_RESET_CLIENT_V2";
	case CONTROL_HARD_RESET_SERVER_V2:
	  return "CONTROL_HARD_RESET_SERVER_V2";
	}
      return NULL;
    }

    std::string dump_packet(const Buffer& buf)
    {
      std::ostringstream out;
      try {
	Buffer b(buf);
	const size_t orig_size = b.size();
	const unsigned int op = b.pop_front();

	const unsigned int opcode = opcode_extract(op);
	const char *op_name = opcode_name(opcode);
	if (op_name)
	  out << op_name << '/' << key_id_extract(op);
	else
	  return "BAD_PACKET";

	if (opcode == DATA_V1 || opcode == DATA_V2)
	  {
	    out << " SIZE=" << b.size() << '/' << orig_size;
	  }
	else
	  {
	    {
	      ProtoSessionID src_psid(b);
	      out << " SRC_PSID=" << src_psid.str();
	    }

	    if (use_tls_auth)
	      {
		const unsigned char *hmac = b.read_alloc(hmac_size);
		out << " HMAC=" << render_hex(hmac, hmac_size);

		PacketID pid;
		pid.read(b, PacketID::LONG_FORM);
		out << " PID=" << pid.str();
	      }

	    ReliableAck ack(0);
	    ack.read(b);
	    const bool dest_psid_defined = !ack.empty();
	    out << " ACK=[";
	    while (!ack.empty())
	      {
		out << " " << ack.front();
		ack.pop_front();
	      }
	    out << " ]";

	    if (dest_psid_defined)
	      {
		ProtoSessionID dest_psid(b);
		out << " DEST_PSID=" << dest_psid.str();
	      }

	    if (opcode != ACK_V1)
	      {
		out << " MSG_ID=" << ReliableAck::read_id(b);
		out << " SIZE=" << b.size() << '/' << orig_size;
	      }
	  }
      }
      catch (const std::exception& e)
	{
	  out << " EXCEPTION: " << e.what();
	}
      return out.str();
    }
#else
    std::string dump_packet(const Buffer& buf)
    {
      return "";
    }
#endif

  protected:

    // used for reading/writing authentication strings (username, password, etc.)

    static void write_string_length(const size_t size, Buffer& buf)
    {
      const boost::uint16_t net_size = htons(size);
      buf.write((const unsigned char *)&net_size, sizeof(net_size));
    }

    static size_t read_string_length(Buffer& buf)
    {
      if (buf.size())
	{
	  boost::uint16_t net_size;
	  buf.read((unsigned char *)&net_size, sizeof(net_size));
	  return ntohs(net_size);
	}
      else
	return 0;
    }

    template <typename S>
    static void write_auth_string(const S& str, Buffer& buf)
    {
      const size_t len = str.length();
      if (len)
	{
	  write_string_length(len+1, buf);
	  buf.write((const unsigned char *)str.c_str(), len);
	  buf.push_back(0);
	}
      else
	write_string_length(0, buf);
    }

    template <typename S>
    static S read_auth_string(Buffer& buf)
    {
      const size_t len = read_string_length(buf);
      if (len)
	{
	  const char *data = (const char *) buf.read_alloc(len);
	  if (len > 1)
	    return S(data, len-1);
	}
      return S();
    }

    template <typename S>
    static void write_control_string(const S& str, Buffer& buf)
    {
      const size_t len = str.length();
      buf.write((const unsigned char *)str.c_str(), len);
      buf.push_back(0);
    }

    template <typename S>
    static S read_control_string(const Buffer& buf)
    {
      size_t size = buf.size();
      if (size)
	{
	  if (buf[size-1] == 0)
	    --size;
	  if (size)
	    return S((const char *)buf.c_data(), size);
	}
      return S();
    }

    template <typename S>
    void write_control_string(const S& str)
    {
      const size_t len = str.length();
      BufferPtr bp = new BufferAllocated(len+1, 0);
      write_control_string(str, *bp);
      control_send(bp);
    }

    static unsigned char *skip_string(Buffer& buf)
    {
      const size_t len = read_string_length(buf);
      return buf.read_alloc(len);
    }

    static void write_empty_string(Buffer& buf)
    {
      write_string_length(0, buf);
    }

    // Packet structure for managing network packets, passed as a template
    // parameter to ProtoStackBase
    class Packet
    {
      friend class ProtoContext;

    public:
      Packet()
      {
	reset_non_buf();
      }

      explicit Packet(const BufferPtr& buf_arg, const unsigned int opcode_arg = CONTROL_V1)
	: opcode(opcode_arg), buf(buf_arg)
      {
      }

      void reset()
      {
	reset_non_buf();
	buf.reset();
      }

      void frame_prepare(const Frame& frame, const unsigned int context)
      {
	if (!buf)
	  buf.reset(new BufferAllocated());
	frame.prepare(context, *buf);
      }

      bool is_raw() const { return opcode != CONTROL_V1; }
      operator bool() const { return bool(buf); }
      const BufferPtr& buffer_ptr() { return buf; }
      const Buffer& buffer() const { return *buf; }

    private:
      void reset_non_buf()
      {
	opcode = INVALID_OPCODE;
      }

      unsigned int opcode;
      BufferPtr buf;
    };

    // KeyContext encapsulates a single SSL/TLS session
    class KeyContext : ProtoStackBase<Packet>, public RC<thread_unsafe_refcount>
    {
      typedef ProtoStackBase<Packet> Base;
      typedef Base::ReliableSend ReliableSend;
      typedef Base::ReliableRecv ReliableRecv;

      // ProtoStackBase protected vars
      using Base::now;
      using Base::rel_recv;
      using Base::rel_send;
      using Base::xmit_acks;

      // ProtoStackBase member functions
      using Base::start_handshake;
      using Base::raw_send;
      using Base::send_pending_acks;

      // Helper for handling deferred data channel setup,
      // for example if cipher/digest are pushed.
      struct DataChannelKey
      {
	DataChannelKey() : rekey_defined(false) {}

	OpenVPNStaticKey key;
	bool rekey_defined;
	CryptoDCInstance::RekeyType rekey_type;
      };

    public:
      typedef boost::intrusive_ptr<KeyContext> Ptr;

      // timeline of events for KeyContext (occurring in order)
      enum EventType {
	KEV_NONE,
	KEV_ACTIVE,         // KeyContext has reached the ACTIVE state
	KEV_NEGOTIATE,      // SSL/TLS negotiation must complete by this time
	KEV_BECOME_PRIMARY, // KeyContext becomes primary for data channel traffic
	KEV_RENEGOTIATE,    // start renegotiating a new KeyContext at this time
	KEV_EXPIRE,         // expiration of KeyContext
	KEV_NEGOTIATE_FAILED, // SSL/TLS negotiation failed
      };

      KeyContext(ProtoContext& p, const bool initiator)
	: Base(*p.config->ssl_factory, p.config->now, p.config->frame, p.stats,
	       p.config->reliable_window, p.config->max_ack_list),
	  proto(p),
	  state(STATE_UNDEF),
	  crypto_flags(0),
	  dirty(0),
	  handled_pid_wrap(false),
	  is_reliable(p.config->protocol.is_reliable()),
	  tlsprf(p.config->tlsprf_factory->new_obj(p.is_server()))
      {
	// set initial state
	set_state((proto.is_server() ? S_INITIAL : C_INITIAL) + (initiator ? 0 : 1));

	// get key_id from parent
	key_id_ = proto.next_key_id();

	// cache stuff that we need to access in hot path
	cache_op32();

	// remember when we were constructed
	construct_time = *now;

	// set must-negotiate-by time
	set_event(KEV_NONE, KEV_NEGOTIATE, construct_time + proto.config->handshake_window);
      }

      // need to call only on the initiator side of the connection
      void start()
      {
	if (state == C_INITIAL || state == S_INITIAL)
	  {
	    send_reset();
	    set_state(state+1);
	    dirty = true;
	  }  
      }

      // control channel flush
      void flush()
      {
	if (dirty)
	  {
	    post_ack_action();
	    Base::flush();
	    send_pending_acks();
	    dirty = false;
	  }
      }

      void invalidate(const Error::Type reason)
      {
	Base::invalidate(reason);
      }

      // retransmit packets as part of reliability layer
      void retransmit()
      {
	// note that we don't set dirty here
	Base::retransmit();
      }

      // when should we next call retransmit method
      Time next_retransmit() const
      {
	const Time t = Base::next_retransmit();
	if (t <= next_event_time)
	  return t;
	else
	  return next_event_time;
      }

      // send app-level cleartext data to peer via SSL
      void app_send(BufferPtr& bp)
      {
	if (state >= ACTIVE)
	  {
	    Base::app_send(bp);
	    dirty = true;
	  }
	else
	  app_pre_write_queue.push_back(bp);
      }

      // pass received ciphertext packets on network to SSL/reliability layers
      bool net_recv(Packet& pkt)
      {
	const bool ret = Base::net_recv(pkt);
	dirty = true;
	return ret;
      }

      // data channel encrypt
      void encrypt(BufferAllocated& buf)
      {
	if (state >= ACTIVE
	    && (crypto_flags & CryptoDCInstance::CRYPTO_DEFINED)
	    && !invalidated())
	  {
	    // compress and encrypt packet and prepend op header
	    const bool pid_wrap = do_encrypt(buf, true);

	    // check for rare situation where packet ID is near overflow
	    test_pid_wrap(pid_wrap);
	  }
	else
	  buf.reset_size(); // no crypto context available
      }

      // data channel decrypt
      void decrypt(BufferAllocated& buf)
      {
	try {
	  if (state >= ACTIVE
	      && (crypto_flags & CryptoDCInstance::CRYPTO_DEFINED)
	      && !invalidated())
	    {
	      // Knock off leading op from buffer, but pass the 32-bit version to
	      // decrypt so it can be used as Additional Data for packet authentication.
	      const size_t head_size = op_head_size(buf[0]);
	      const unsigned char *op32 = (head_size == OP_SIZE_V2) ? buf.c_data() : NULL;
	      buf.advance(head_size);

	      // decrypt packet
	      const Error::Type err = crypto->decrypt(buf, now->seconds_since_epoch(), op32);
	      if (err)
		{
		  proto.stats->error(err);
		  if (proto.is_tcp() && (err == Error::DECRYPT_ERROR || err == Error::HMAC_ERROR))
		    invalidate(err);
		}

	      // decompress packet
	      if (compress)
		compress->decompress(buf);
	    }
	  else
	    buf.reset_size(); // no crypto context available
	}
	catch (BufferException&)
	  {
	    proto.stats->error(Error::BUFFER_ERROR);
	    buf.reset_size();
	    if (proto.is_tcp())
	      invalidate(Error::BUFFER_ERROR);
	  }
      }

      // usually called by parent ProtoContext object when this KeyContext
      // has been retired.
      void prepare_expire()
      {
	set_event(KEV_NONE, KEV_EXPIRE, construct_time + proto.config->expire);
      }

      // is an KEV_x event pending?
      bool event_pending()
      {
	if (current_event == KEV_NONE && *now >= next_event_time)
	  process_next_event();
	return current_event != KEV_NONE;
      }

      // get KEV_x event
      EventType get_event() const { return current_event; }

      // clear KEV_x event
      void reset_event() { set_event(KEV_NONE); }

      // was session invalidated by an exception?
      bool invalidated() const { return Base::invalidated(); }

      // Reason for invalidation
      Error::Type invalidation_reason() const { return Base::invalidation_reason(); }

      // our Key ID in the OpenVPN protocol
      unsigned int key_id() const { return key_id_; }

      // indicates that data channel is keyed and ready to encrypt/decrypt packets
      bool data_channel_ready() const { return state >= ACTIVE; }

      bool is_dirty() const { return dirty; }

      // notification from parent of rekey operation
      void rekey(const CryptoDCInstance::RekeyType type)
      {
	if (crypto)
	  crypto->rekey(type);
	else if (data_channel_key.defined())
	  {
	    // save for deferred processing
	    data_channel_key->rekey_type = type;
	    data_channel_key->rekey_defined = true;
	  }
      }

      // time that our state transitioned to ACTIVE
      Time reached_active() const { return reached_active_time_; }

      // transmit a keepalive message to peer
      void send_keepalive()
      {
	send_data_channel_message(proto_context_private::keepalive_message,
				  sizeof(proto_context_private::keepalive_message));
      }

      // send explicit-exit-notify message to peer
      void send_explicit_exit_notify()
      {
	send_data_channel_message(proto_context_private::explicit_exit_notify_message,
				  sizeof(proto_context_private::explicit_exit_notify_message));
      }

      // general purpose method for sending constant string messages
      // to peer via data channel
      void send_data_channel_message(const unsigned char *data, const size_t size)
      {
	if (state >= ACTIVE
	    && (crypto_flags & CryptoDCInstance::CRYPTO_DEFINED)
	    && !invalidated())
	  {
	    // allocate packet
	    Packet pkt;
	    pkt.frame_prepare(*proto.config->frame, Frame::WRITE_DC_MSG);

	    // write keepalive message
	    pkt.buf->write(data, size);

	    // process packet for transmission
	    do_encrypt(*pkt.buf, false); // set compress hint to "no"

	    // send it
	    proto.net_send(key_id_, pkt);
	  }
      }

      // validate the integrity of a packet
      static bool validate(const Buffer& net_buf, ProtoContext& proto, TimePtr now)
      {
	try {
	  Buffer recv(net_buf);
	  if (proto.use_tls_auth)
	    {
	      const unsigned char *orig_data = recv.data();
	      const size_t orig_size = recv.size();

	      // advance buffer past initial op byte
	      recv.advance(1);

	      // get source PSID
	      ProtoSessionID src_psid(recv);

	      // verify HMAC
	      {
		recv.advance(proto.hmac_size);
		if (!proto.ta_hmac_recv->ovpn_hmac_cmp(orig_data, orig_size,
						       1 + ProtoSessionID::SIZE,
						       proto.hmac_size,
						       PacketID::size(PacketID::LONG_FORM)))
		  return false;
	      }

	      // verify source PSID
	      if (!proto.psid_peer.match(src_psid))
		return false;

	      // read tls_auth packet ID
	      const PacketID pid = proto.ta_pid_recv.read_next(recv);

	      // get current time_t
	      const PacketID::time_t t = now->seconds_since_epoch();

	      // verify tls_auth packet ID
	      const bool pid_ok = proto.ta_pid_recv.test(pid, t);

	      // make sure that our own PSID is contained in packet received from peer
	      if (ReliableAck::ack_skip(recv))
		{
		  ProtoSessionID dest_psid(recv);
		  if (!proto.psid_self.match(dest_psid))
		    return false;
		}

	      return pid_ok;
	    }
	  else
	    {
	      // advance buffer past initial op byte
	      recv.advance(1);

	      // verify source PSID
	      ProtoSessionID src_psid(recv);
	      if (!proto.psid_peer.match(src_psid))
		return false;

	      // make sure that our own PSID is contained in packet received from peer
	      if (ReliableAck::ack_skip(recv))
		{
		  ProtoSessionID dest_psid(recv);
		  if (!proto.psid_self.match(dest_psid))
		    return false;
		}

	      return true;
	    }
	}
	catch (BufferException&)
	  {
	    return false;
	  }
      }

      // Initialize the components of the OpenVPN data channel protocol
      void init_data_channel()
      {
	// set up crypto for data channel
	if (data_channel_key.defined())
	  {
	    bool enable_compress = true;
	    Config& c = *proto.config;
	    const unsigned int key_dir = proto.is_server() ? OpenVPNStaticKey::INVERSE : OpenVPNStaticKey::NORMAL;
	    const OpenVPNStaticKey& key = data_channel_key->key;

	    // build crypto context for data channel encryption/decryption
	    crypto = c.dc.context().new_obj(key_id_);
	    crypto_flags = crypto->defined();

	    if (crypto_flags & CryptoDCInstance::CIPHER_DEFINED)
	      crypto->init_cipher(key.slice(OpenVPNStaticKey::CIPHER | OpenVPNStaticKey::ENCRYPT | key_dir),
				  key.slice(OpenVPNStaticKey::CIPHER | OpenVPNStaticKey::DECRYPT | key_dir));

	    if (crypto_flags & CryptoDCInstance::HMAC_DEFINED)
	      crypto->init_hmac(key.slice(OpenVPNStaticKey::HMAC | OpenVPNStaticKey::ENCRYPT | key_dir),
				key.slice(OpenVPNStaticKey::HMAC | OpenVPNStaticKey::DECRYPT | key_dir));

	    crypto->init_pid(PacketID::SHORT_FORM,
			     c.pid_mode,
			     PacketID::SHORT_FORM,
			     c.pid_seq_backtrack, c.pid_time_backtrack,
			     "DATA", int(key_id_),
			     proto.stats);

	    enable_compress = crypto->consider_compression(proto.config->comp_ctx);

	    if (data_channel_key->rekey_defined)
	      crypto->rekey(data_channel_key->rekey_type);
	    data_channel_key.reset();

	    // set up compression for data channel
	    if (enable_compress)
	      compress = proto.config->comp_ctx.new_compressor(proto.config->frame, proto.stats);
	    else
	      compress.reset();

	    // cache op32 for hot path in do_encrypt
	    cache_op32();
	  }
      }

    private:
      bool do_encrypt(BufferAllocated& buf, const bool compress_hint)
      {
	bool pid_wrap;

	// compress packet
	if (compress)
	  compress->compress(buf, compress_hint);

	if (enable_op32)
	  {
	    const boost::uint32_t op32 = htonl(op32_compose(DATA_V2, key_id_, remote_peer_id));

	    static_assert(sizeof(op32) == OP_SIZE_V2, "OP_SIZE_V2 inconsistency");

	    // encrypt packet
	    pid_wrap = crypto->encrypt(buf, now->seconds_since_epoch(), (const unsigned char *)&op32);

	    // prepend op
	    buf.prepend((const unsigned char *)&op32, sizeof(op32));
	  }
	else
	  {
	    // encrypt packet
	    pid_wrap = crypto->encrypt(buf, now->seconds_since_epoch(), NULL);

	    // prepend op
	    buf.push_front(op_compose(DATA_V1, key_id_));
	  }
	return pid_wrap;
      }

      // cache op32 and remote_peer_id
      void cache_op32()
      {
	enable_op32 = proto.config->enable_op32;
	remote_peer_id = proto.config->remote_peer_id;
      }

      void set_state(const int newstate)
      {
	OPENVPN_LOG_PROTO_VERBOSE("KeyContext " << (proto.is_server() ? "SERVER " : "CLIENT ") << state_string(state) << " -> " << state_string(newstate));
	state = newstate;
      }

      void set_event(const EventType current)
      {
	OPENVPN_LOG_PROTO_VERBOSE("KeyContext " << event_type_string(current));
	current_event = current;
      }

      void set_event(const EventType next, const Time& next_time)
      {
	OPENVPN_LOG_PROTO_VERBOSE("KeyContext " << event_type_string(next) << '(' << seconds_until(next_time) << ')');
	next_event = next;
	next_event_time = next_time;
      }

      void set_event(const EventType current, const EventType next, const Time& next_time)
      {
	OPENVPN_LOG_PROTO_VERBOSE("KeyContext " << event_type_string(current) << " -> " << event_type_string(next) << '(' << seconds_until(next_time) << ')');
	current_event = current;
	next_event = next;
	next_event_time = next_time;
      }

      // called by ProtoStackBase when session is invalidated
      virtual void invalidate_callback()
      {
	reached_active_time_ = Time();
	set_event(KEV_NONE, Time::infinite());
      }

      // Trigger a new SSL/TLS negotiation if packet ID (a 32-bit unsigned int)
      // is getting close to wrapping around.  If it wraps back to 0 without
      // a renegotiation, it would cause the relay protection logic to wrongly
      // think that all further packets are replays.
      void test_pid_wrap(const bool pid_wrap)
      {
	if (pid_wrap && !handled_pid_wrap)
	  {
	    trigger_renegotiation();
	    handled_pid_wrap = true;
	  }
      }

      void trigger_renegotiation()
      {
	if (state >= ACTIVE && !invalidated())
	  set_event(KEV_RENEGOTIATE, KEV_EXPIRE, construct_time + proto.config->expire);
      }

      void active_event()
      {
	set_event(KEV_ACTIVE, KEV_BECOME_PRIMARY, construct_time + proto.config->become_primary);
      }

      void process_next_event()
      {
	if (*now >= next_event_time)
	  {
	    switch (next_event)
	      {
	      case KEV_NEGOTIATE:
		if (state >= ACTIVE)
		  set_event(KEV_NEGOTIATE, KEV_BECOME_PRIMARY, construct_time + proto.config->become_primary);
		else
		  {
		    invalidate(Error::KEV_NEGOTIATE_ERROR);
		    set_event(KEV_NEGOTIATE_FAILED);
		  }
		break;
	      case KEV_BECOME_PRIMARY:
		set_event(KEV_BECOME_PRIMARY, KEV_RENEGOTIATE, construct_time + proto.config->renegotiate);
		break;
	      case KEV_RENEGOTIATE:
		set_event(KEV_RENEGOTIATE, KEV_EXPIRE, construct_time + proto.config->expire);
		break;
	      case KEV_EXPIRE:
		invalidate(Error::KEV_EXPIRE_ERROR);
		set_event(KEV_EXPIRE);
		break;
	      default:
		break;
	      }
	  }
      }

      unsigned int initial_op(const bool sender) const
      {
	if (key_id_)
	  return CONTROL_SOFT_RESET_V1;
	else
	  return (proto.is_server() == sender) ? CONTROL_HARD_RESET_SERVER_V2 : CONTROL_HARD_RESET_CLIENT_V2;
      }

      void send_reset()
      {
	Packet pkt;
	pkt.opcode = initial_op(true);
	pkt.frame_prepare(*proto.config->frame, Frame::WRITE_SSL_INIT);
	raw_send(pkt);
      }

      virtual void raw_recv(Packet& raw_pkt)
      {
	if (raw_pkt.buf->empty() && raw_pkt.opcode == initial_op(false))
	  {
	    switch (state)
	      {
	      case C_WAIT_RESET:
		send_reset();
		set_state(C_WAIT_RESET_ACK);
		break;
	      case S_WAIT_RESET:
		send_reset();
		set_state(S_WAIT_RESET_ACK);
		break;
	      }
	  }
      }

      virtual void app_recv(BufferPtr& to_app_buf)
      {
	switch (state)
	  {
	  case C_WAIT_AUTH:
	    recv_auth(*to_app_buf);
	    set_state(C_WAIT_AUTH_ACK);
	    break;
	  case S_WAIT_AUTH:
	    recv_auth(*to_app_buf);
	    send_auth();	
	    set_state(S_WAIT_AUTH_ACK);
	    break;
	  case S_WAIT_AUTH_ACK: // rare case where client receives auth, goes ACTIVE, but the ACK response is dropped
	  case ACTIVE:
	    proto.app_recv(key_id_, to_app_buf);
	    break;
	  }
      }

      virtual void net_send(const Packet& net_pkt, const Base::NetSendType nstype)
      {
	if (!is_reliable || nstype != Base::NET_SEND_RETRANSMIT) // retransmit packets on UDP only, not TCP
	  proto.net_send(key_id_, net_pkt);
      }

      void post_ack_action()
      {
	if (state <= LAST_ACK_STATE && !rel_send.n_unacked())
	  {
	    switch (state)
	      {
	      case C_WAIT_RESET_ACK:
		start_handshake();
		send_auth();
		set_state(C_WAIT_AUTH);
		break;
	      case S_WAIT_RESET_ACK:
		start_handshake();
		set_state(S_WAIT_AUTH);
		break;
	      case C_WAIT_AUTH_ACK:
		active();
		set_state(ACTIVE);
		break;
	      case S_WAIT_AUTH_ACK:
		active();
		set_state(ACTIVE);
		break;
	      }
	  }
      }

      void send_auth()
      {
	BufferPtr buf = new BufferAllocated();
	proto.config->frame->prepare(Frame::WRITE_SSL_CLEARTEXT, *buf);
	buf->write(proto_context_private::auth_prefix, sizeof(proto_context_private::auth_prefix));
	tlsprf->self_randomize(*proto.config->rng);
	tlsprf->self_write(*buf);
	const std::string options = proto.config->options_string();
	write_auth_string(options, *buf);
	if (!proto.is_server())
	  {
	    OPENVPN_LOG_PROTO("Tunnel Options:" << options);
	    buf->or_flags(BufferAllocated::DESTRUCT_ZERO);
	    if (proto.config->xmit_creds)
	      proto.client_auth(*buf);
	    else
	      {
		write_empty_string(*buf); // username
		write_empty_string(*buf); // password
	      }
	    const std::string peer_info = proto.config->peer_info_string();
	    write_auth_string(peer_info, *buf);
	  }
	Base::app_send(buf);
	dirty = true;
      }

      void recv_auth(BufferAllocated& buf)
      {
	const unsigned char *buf_pre = buf.read_alloc(sizeof(proto_context_private::auth_prefix));
	if (std::memcmp(buf_pre, proto_context_private::auth_prefix, sizeof(proto_context_private::auth_prefix)))
	  throw bad_auth_prefix();
	tlsprf->peer_read(buf);
	const std::string options = read_auth_string<std::string>(buf);
	if (proto.is_server())
	  {
	    const std::string username = read_auth_string<std::string>(buf);
	    const SafeString password = read_auth_string<SafeString>(buf);
	    const std::string peer_info = read_auth_string<std::string>(buf);
	    proto.server_auth(username, password, peer_info);
	  }
      }

      void active()
      {
	OPENVPN_LOG_SSL("SSL Handshake: " << Base::ssl_handshake_details());
	generate_session_keys();
	while (!app_pre_write_queue.empty())
	  {
	    Base::app_send(app_pre_write_queue.front());
	    app_pre_write_queue.pop_front();
	    dirty = true;
	  }
	reached_active_time_ = *now;
	proto.slowest_handshake_.max(reached_active_time_ - construct_time);
	active_event();
      }

      // use the TLS PRF construction to exchange session keys for building
      // the data channel crypto context
      void generate_session_keys()
      {
	ScopedPtr<DataChannelKey> dck(new DataChannelKey());
	tlsprf->generate_key_expansion(dck->key, proto.psid_self, proto.psid_peer);
	OPENVPN_LOG_PROTO_VERBOSE("KEY " << proto.mode().str() << ' ' << dck->key.render());
	tlsprf->erase();
	dck.swap(data_channel_key);
	if (!proto.dc_deferred)
	  init_data_channel();
      }

      // generate message head
      void gen_head(const unsigned int opcode, Buffer& buf)
      {
	if (proto.use_tls_auth)
	  {
	    // write tls-auth packet ID
	    proto.ta_pid_send.write_next(buf, true, now->seconds_since_epoch());

	    // make space for tls-auth HMAC
	    buf.prepend_alloc(proto.hmac_size);

	    // write source PSID
	    proto.psid_self.prepend(buf);

	    // write opcode
	    buf.push_front(op_compose(opcode, key_id_));

	    // write hmac
	    proto.ta_hmac_send->ovpn_hmac_gen(buf.data(), buf.size(),
					      1 + ProtoSessionID::SIZE,
					      proto.hmac_size,
					      PacketID::size(PacketID::LONG_FORM));
	  }
	else
	  {
	    // write source PSID
	    proto.psid_self.prepend(buf);

	    // write opcode
	    buf.push_front(op_compose(opcode, key_id_));
	  }
      }

      void prepend_dest_psid_and_acks(Buffer& buf)
      {
	// if sending ACKs, prepend dest PSID
	if (!xmit_acks.empty())
	  {
	    if (proto.psid_peer.defined())
	      proto.psid_peer.prepend(buf);
	    else
	      {
		proto.stats->error(Error::CC_ERROR);
		throw peer_psid_undef();
	      }
	  }

	// prepend ACKs for messages received from peer
	xmit_acks.prepend(buf);
      }

      bool verify_src_psid(const ProtoSessionID& src_psid)
      {
	if (proto.psid_peer.defined())
	  {
	    if (!proto.psid_peer.match(src_psid))
	      {
		proto.stats->error(Error::CC_ERROR);
		if (proto.is_tcp())
		  invalidate(Error::CC_ERROR);
		return false;
	      }
	  }
	else
	  {
	    proto.psid_peer = src_psid;
	  }
	return true;
      }

      bool verify_dest_psid(Buffer& buf)
      {
	ProtoSessionID dest_psid(buf);
	if (!proto.psid_self.match(dest_psid))
	  {
	    proto.stats->error(Error::CC_ERROR);
	    if (proto.is_tcp())
	      invalidate(Error::CC_ERROR);
	    return false;
	  }
	return true;
      }

      virtual void encapsulate(id_t id, Packet& pkt)
      {
	Buffer& buf = *pkt.buf;

	// prepend message sequence number
	ReliableAck::prepend_id(buf, id);

	// prepend dest PSID and ACKs to reply to peer
	prepend_dest_psid_and_acks(buf);

	// generate message head
	gen_head(pkt.opcode, buf);
      }

      virtual bool decapsulate(Packet& pkt)
      {
	try {
	  Buffer& recv = *pkt.buf;

	  if (proto.use_tls_auth)
	    {
	      const unsigned char *orig_data = recv.data();
	      const size_t orig_size = recv.size();

	      // advance buffer past initial op byte
	      recv.advance(1);

	      // get source PSID
	      ProtoSessionID src_psid(recv);

	      // verify HMAC
	      {
		recv.advance(proto.hmac_size);
		if (!proto.ta_hmac_recv->ovpn_hmac_cmp(orig_data, orig_size,
						       1 + ProtoSessionID::SIZE,
						       proto.hmac_size,
						       PacketID::size(PacketID::LONG_FORM)))
		  {
		    proto.stats->error(Error::HMAC_ERROR);
		    if (proto.is_tcp())
		      invalidate(Error::HMAC_ERROR);
		    return false;
		  }      
	      }

	      // update our last-packet-received time
	      proto.update_last_received();

	      // verify source PSID
	      if (!verify_src_psid(src_psid))
		return false;

	      // read tls_auth packet ID
	      const PacketID pid = proto.ta_pid_recv.read_next(recv);

	      // get current time_t
	      const PacketID::time_t t = now->seconds_since_epoch();

	      // verify tls_auth packet ID
	      const bool pid_ok = proto.ta_pid_recv.test(pid, t);

	      // process ACKs sent by peer (if packet ID check failed,
	      // read the ACK IDs, but don't modify the rel_send object).
	      if (ReliableAck::ack(rel_send, recv, pid_ok))
		{
		  // make sure that our own PSID is contained in packet received from peer
		  if (!verify_dest_psid(recv))
		    return false;
		}

	      // for CONTROL packets only, not ACK
	      if (pkt.opcode != ACK_V1)
		{
		  // get message sequence number
		  const id_t id = ReliableAck::read_id(recv);

		  if (pid_ok)
		    {
		      // try to push message into reliable receive object
		      const unsigned int rflags = rel_recv.receive(pkt, id);

		      // should we ACK packet back to sender?
		      if (rflags & ReliableRecv::ACK_TO_SENDER)
			xmit_acks.push_back(id); // ACK packet to sender

		      // was packet accepted by reliable receive object?
		      if (rflags & ReliableRecv::IN_WINDOW)
			{
			  proto.ta_pid_recv.add(pid, t); // remember tls_auth packet ID so that it can't be replayed
			  return true;
			}
		    }
		  else // treat as replay
		    {
		      proto.stats->error(Error::REPLAY_ERROR);
		      if (pid.is_valid())
			xmit_acks.push_back(id); // even replayed packets must be ACKed or protocol could deadlock
		    }
		}
	      else
		{
		  if (pid_ok)
		    proto.ta_pid_recv.add(pid, t); // remember tls_auth packet ID of ACK packet to prevent replay
		  else
		    proto.stats->error(Error::REPLAY_ERROR);
		}
	    }
	  else // non tls_auth mode
	    {
	      // update our last-packet-received time
	      proto.update_last_received();

	      // advance buffer past initial op byte
	      recv.advance(1);

	      // verify source PSID
	      ProtoSessionID src_psid(recv);
	      if (!verify_src_psid(src_psid))
		return false;

	      // process ACKs sent by peer
	      if (ReliableAck::ack(rel_send, recv, true))
		{
		  // make sure that our own PSID is in packet received from peer
		  if (!verify_dest_psid(recv))
		    return false;
		}

	      // for CONTROL packets only, not ACK
	      if (pkt.opcode != ACK_V1)
		{
		  // get message sequence number
		  const id_t id = ReliableAck::read_id(recv);

		  // try to push message into reliable receive object
		  const unsigned int rflags = rel_recv.receive(pkt, id);

		  // should we ACK packet back to sender?
		  if (rflags & ReliableRecv::ACK_TO_SENDER)
		    xmit_acks.push_back(id); // ACK packet to sender

		  // was packet accepted by reliable receive object?
		  if (rflags & ReliableRecv::IN_WINDOW)
		    return true;
		}
	    }
	}
	catch (BufferException&)
	  {
	    proto.stats->error(Error::BUFFER_ERROR);
	    if (proto.is_tcp())
	      invalidate(Error::BUFFER_ERROR);
	  }
	return false;
      }

      virtual void generate_ack(Packet& pkt)
      {
	Buffer& buf = *pkt.buf;

	// prepend dest PSID and ACKs to reply to peer
	prepend_dest_psid_and_acks(buf);

	// generate message head
	gen_head(ACK_V1, buf);
      }

      // for debugging
      static const char *event_type_string(const EventType et)
      {
	switch (et)
	  {
	  case KEV_NONE:
	    return "KEV_NONE";
	  case KEV_ACTIVE:
	    return "KEV_ACTIVE";
	  case KEV_NEGOTIATE:
	    return "KEV_NEGOTIATE";
	  case KEV_BECOME_PRIMARY:
	    return "KEV_BECOME_PRIMARY";
	  case KEV_RENEGOTIATE:
	    return "KEV_RENEGOTIATE";
	  case KEV_EXPIRE:
	    return "KEV_EXPIRE";
	  case KEV_NEGOTIATE_FAILED:
	    return "KEV_NEGOTIATE_FAILED";
	  default:
	    return "KEV_?";
	  }
      }

      // for debugging
      static const char *state_string(const int s)
      {
	switch (s)
	  {
	  case C_WAIT_RESET_ACK:
	    return "C_WAIT_RESET_ACK";
	  case C_WAIT_AUTH_ACK:
	    return "C_WAIT_AUTH_ACK";
	  case S_WAIT_RESET_ACK:
	    return "S_WAIT_RESET_ACK";
	  case S_WAIT_AUTH_ACK:
	    return "S_WAIT_AUTH_ACK";
	  case C_INITIAL:
	    return "C_INITIAL";
	  case C_WAIT_RESET:
	    return "C_WAIT_RESET";
	  case C_WAIT_AUTH:
	    return "C_WAIT_AUTH";
	  case S_INITIAL:
	    return "S_INITIAL";
	  case S_WAIT_RESET:
	    return "S_WAIT_RESET";
	  case S_WAIT_AUTH:
	    return "S_WAIT_AUTH";
	  case ACTIVE:
	    return "ACTIVE";
	  default:
	    return "STATE_UNDEF";
	  }
      }

      // for debugging
      int seconds_until(const Time& next_time)
      {
	Time::Duration d = next_time - *now;
	if (d.is_infinite())
	  return -1;
	else
	  return d.to_seconds();
      }

      // BEGIN KeyContext data members

      ProtoContext& proto; // parent
      int state;
      unsigned int key_id_;
      unsigned int crypto_flags;
      int remote_peer_id; // -1 to disable
      bool enable_op32;
      bool dirty;
      bool handled_pid_wrap;
      bool is_reliable;
      Compress::Ptr compress;
      CryptoDCInstance::Ptr crypto;
      TLSPRFInstance::Ptr tlsprf;
      Time construct_time;
      Time reached_active_time_;
      Time next_event_time;
      EventType current_event;
      EventType next_event;
      std::deque<BufferPtr> app_pre_write_queue;
      ScopedPtr<DataChannelKey> data_channel_key;
    };

  public:

    // Validate the integrity of a packet, only considering tls-auth HMAC.
    class TLSAuthPreValidate : public RC<thread_unsafe_refcount>
    {
    public:
      typedef boost::intrusive_ptr<TLSAuthPreValidate> Ptr;

      OPENVPN_SIMPLE_EXCEPTION(tls_auth_pre_validate);

      TLSAuthPreValidate(const Config& c, const bool server)
      {
	if (!c.tls_auth_enabled())
	  throw tls_auth_pre_validate();

	// init OvpnHMACInstance
	ta_hmac_recv = c.tls_auth_context->new_obj();

	// save hard reset op we expect to receive from peer
	reset_op = server ? CONTROL_HARD_RESET_CLIENT_V2 : CONTROL_HARD_RESET_SERVER_V2;

	// init tls_auth hmac
	if (c.key_direction >= 0)
	  {
	    // key-direction is 0 or 1
	    const unsigned int key_dir = c.key_direction ? OpenVPNStaticKey::INVERSE : OpenVPNStaticKey::NORMAL;
	    ta_hmac_recv->init(c.tls_auth_key.slice(OpenVPNStaticKey::HMAC | OpenVPNStaticKey::DECRYPT | key_dir));
	  }
	else
	  {
	    // key-direction bidirectional mode
	    ta_hmac_recv->init(c.tls_auth_key.slice(OpenVPNStaticKey::HMAC));
	  }
      }

      bool validate(const Buffer& net_buf)
      {
	try {
	  if (net_buf.size())
	    {
	      const unsigned int op = net_buf[0];
	      if (opcode_extract(op) != reset_op || key_id_extract(op) != 0)
		return false;
	      return ta_hmac_recv->ovpn_hmac_cmp(net_buf.c_data(), net_buf.size(),
						 1 + ProtoSessionID::SIZE,
						 ta_hmac_recv->output_size(),
						 PacketID::size(PacketID::LONG_FORM));
	    }
	}
	catch (BufferException&)
	  {
	  }
	return false;
      }

    private:
      OvpnHMACInstance::Ptr ta_hmac_recv;
      unsigned int reset_op;
    };

    OPENVPN_SIMPLE_EXCEPTION(select_key_context_error);

    ProtoContext(const Config::Ptr& config_arg,             // configuration
		 const SessionStats::Ptr& stats_arg)        // error stats
      : config(config_arg),
	stats(stats_arg),
	mode_(config_arg->ssl_factory->mode()),
	n_key_ids(0),
	now_(config_arg->now)
    {
      const Config& c = *config;

      // tls-auth setup
      if (c.tls_auth_context)
	{
	  use_tls_auth = true;

	  // get HMAC size from Digest object
	  hmac_size = c.tls_auth_context->size();
	}
      else
	{
	  use_tls_auth = false;
	  hmac_size = 0;
	}
    }

    void reset()
    {
      const Config& c = *config;

      // validate options
      c.validate_complete();

      // by default, fast_transition is turned off
      fast_transition = false;

      // defer data channel initialization until after client options pull?
      dc_deferred = c.dc_deferred;

      // clear key contexts
      reset_all();

      // start with key ID 0
      upcoming_key_id = 0;

      // tls-auth initialization
      if (use_tls_auth)
	{
	  // init OvpnHMACInstance
	  ta_hmac_send = c.tls_auth_context->new_obj();
	  ta_hmac_recv = c.tls_auth_context->new_obj();

	  // init tls_auth hmac
	  if (c.key_direction >= 0)
	    {
	      // key-direction is 0 or 1
	      const unsigned int key_dir = c.key_direction ? OpenVPNStaticKey::INVERSE : OpenVPNStaticKey::NORMAL;
	      ta_hmac_send->init(c.tls_auth_key.slice(OpenVPNStaticKey::HMAC | OpenVPNStaticKey::ENCRYPT | key_dir));
	      ta_hmac_recv->init(c.tls_auth_key.slice(OpenVPNStaticKey::HMAC | OpenVPNStaticKey::DECRYPT | key_dir));
	    }
	  else
	    {
	      // key-direction bidirectional mode
	      ta_hmac_send->init(c.tls_auth_key.slice(OpenVPNStaticKey::HMAC));
	      ta_hmac_recv->init(c.tls_auth_key.slice(OpenVPNStaticKey::HMAC));
	    }

	  // init tls_auth packet ID
	  ta_pid_send.init(PacketID::LONG_FORM);
	  ta_pid_recv.init(c.pid_mode,
			   PacketID::LONG_FORM,
			   c.pid_seq_backtrack, c.pid_time_backtrack,
			   "SSL-CC", 0,
			   stats);
	}

      // initialize proto session ID
      psid_self.randomize(*c.prng);
      psid_peer.reset();

      // initialize key contexts
      primary.reset(new KeyContext(*this, is_client()));

      // initialize keepalive timers
      keepalive_expire = Time::infinite();   // initially disabled
      update_last_sent();                    // set timer for initial keepalive send
    }

    // Free up space when parent object has been halted but
    // object destruction is not immediately scheduled.
    void pre_destroy()
    {
      reset_all();
    }

    virtual ~ProtoContext() {}

    // return the PacketType of an incoming network packet
    PacketType packet_type(const Buffer& buf)
    {
      return PacketType(buf, *this);
    }

    // start protocol negotiation
    void start()
    {
      primary->start();
      update_last_received(); // set an upper bound on when we expect a response
    }

    // trigger a protocol renegotiation
    void renegotiate()
    {
      // initialize secondary key context
      secondary.reset(new KeyContext(*this, true));
      secondary->start();
    }

    // Should be called at the end of sequence of send/recv
    // operations on underlying protocol object.
    // If control_channel is true, do a full flush.
    // If control_channel is false, optimize flush for data
    // channel only.
    void flush(const bool control_channel)
    {
      if (control_channel || process_events())
	{
	  do {
	    primary->flush();
	    if (secondary)
	      secondary->flush();
	  } while (process_events());
	}
    }

    // Perform various time-based housekeeping tasks such as retransmiting
    // unacknowleged packets as part of the reliability layer and testing
    // for keepalive timouts.
    // Should be called at the time returned by next_housekeeping.
    void housekeeping()
    {
      // handle control channel retransmissions on primary
      primary->retransmit();

      // handle control channel retransmissions on secondary
      if (secondary)
	secondary->retransmit();

      // handle possible events
      flush(false);

      // handle keepalive/expiration
      keepalive_housekeeping();
    }

    // When should we next call housekeeping?
    // Will return a time value for immediate execution
    // if session has been invalidated.
    Time next_housekeeping() const
    {
      if (!invalidated())
	{
	  Time ret = primary->next_retransmit();
	  if (secondary)
	    ret.min(secondary->next_retransmit());
	  ret.min(keepalive_xmit);
	  ret.min(keepalive_expire);
	  return ret;
	}
      else
	return Time();
    }

    // send app-level cleartext to remote peer

    void control_send(BufferPtr& app_bp)
    {
      select_control_send_context().app_send(app_bp);
      app_bp.reset();
    }

    void control_send(BufferAllocated& app_buf)
    {
      BufferPtr bp = new BufferAllocated();
      bp->move(app_buf);
      control_send(bp);
    }

    // validate a control channel network packet
    bool control_net_validate(const PacketType& type, const Buffer& net_buf)
    {
      return type.is_defined() && KeyContext::validate(net_buf, *this, now_);
    }

    // pass received control channel network packets (ciphertext) into protocol object

    bool control_net_recv(const PacketType& type, BufferAllocated& net_buf)
    {
      BufferPtr bp = new BufferAllocated();
      bp->move(net_buf);
      Packet pkt(bp, type.opcode);
      if (type.is_soft_reset() && !renegotiate_request(pkt))
	return false;
      return select_key_context(type, true).net_recv(pkt);
    }

    bool control_net_recv(const PacketType& type, BufferPtr& net_bp)
    {
      Packet pkt(net_bp, type.opcode);
      if (type.is_soft_reset() && !renegotiate_request(pkt))
	return false;
      return select_key_context(type, true).net_recv(pkt);
    }

    // encrypt a data channel packet using primary KeyContext
    void data_encrypt(BufferAllocated& in_out)
    {
      primary->encrypt(in_out);
    }

    // decrypt a data channel packet (automatically select primary
    // or secondary KeyContext based on packet content)
    bool data_decrypt(const PacketType& type, BufferAllocated& in_out)
    {
      bool ret = false;

      select_key_context(type, false).decrypt(in_out);

      // update time of most recent packet received
      if (in_out.size())
	{
	  update_last_received();
	  ret = true;
	}

      // discard keepalive packets
      if (proto_context_private::is_keepalive(in_out))
	{
	  in_out.reset_size();
	}

      return ret;
    }

    // enter disconnected state
    void disconnect(const Error::Type reason)
    {
      primary->invalidate(reason);
      if (secondary)
	secondary->invalidate(reason);
    }

    // normally used by UDP clients to tell the server that
    // they are disconnecting
    void send_explicit_exit_notify()
    {
      if (is_client() && is_udp())
	primary->send_explicit_exit_notify();
    }

    // should be called after a successful network packet transmit
    void update_last_sent()
    {
      keepalive_xmit = *now_ + config->keepalive_ping;
    }

    // can we call data_encrypt or data_decrypt yet?
    bool data_channel_ready() const { return primary->data_channel_ready(); }

    // total number of SSL/TLS negotiations during lifetime of ProtoContext object
    unsigned int negotiations() const { return n_key_ids; }

    // worst-case handshake time
    const Time::Duration& slowest_handshake() { return slowest_handshake_; }

    // was primary context invalidated by an exception?
    bool invalidated() const { return primary->invalidated(); }

    // reason for invalidation if invalidated() above returns true
    Error::Type invalidation_reason() const { return primary->invalidation_reason(); }

    // Enable original OpenVPN behavior of switching to new SSL/TLS key
    // context for control channel sends immediately after renegotiation
    // reaches ACTIVE state.  By default, we will transition to new
    // key context over the "become_primary" duration in Config.
    // The default behavior is known to be more reliable, but the
    // original behavior may be enabled by calling this method.
    void enable_fast_transition() { fast_transition = true; }

    // A placeholder for enabling strict OpenVPN 2.x protocol
    // compatibility.
    void enable_strict_openvpn_2x()
    {
      enable_fast_transition();
    }

    // Do late initialization of data channel, for example
    // on client after server push, or on server after client
    // capabilities are known.
    void init_data_channel()
    {
      dc_deferred = false;

      // initialize data channel (crypto & compression)
      primary->init_data_channel();
      if (secondary)
	secondary->init_data_channel();
    }

    // Call on client with server-pushed options
    void process_push(const OptionList& opt, const ProtoContextOptions& pco)
    {
      // modify config with pushed options
      config->process_push(opt, pco);
      init_data_channel();

      // in case keepalive parms were modified by push
      keepalive_parms_modified();
    }

    // Return the current transport alignment adjustment
    size_t align_adjust_hint() const
    {
      return config->enable_op32 ? 0 : 1;
    }

    // Disable keepalive for rest of session,
    // but return the previous keepalive parameters.
    void disable_keepalive(unsigned int& keepalive_ping,
			   unsigned int& keepalive_timeout)
    {
      keepalive_ping = config->keepalive_ping.to_seconds();
      keepalive_timeout = config->keepalive_timeout.to_seconds();
      config->keepalive_ping = Time::Duration::infinite();
      config->keepalive_timeout = Time::Duration::infinite();
      keepalive_parms_modified();
    }

    // override the data channel factory
    void override_dc_factory(const CryptoDCFactory::Ptr& dc_factory)
    {
      config->dc.set_factory(dc_factory);
    }

    // set the local peer ID (or -1 to disable)
    void set_local_peer_id(const int local_peer_id)
    {
      config->local_peer_id = local_peer_id;
    }

    // current time
    const Time& now() const { return *now_; }
    void update_now() { now_->update(); }

    // frame
    const Frame& frame() const { return *config->frame; }
    const Frame::Ptr& frameptr() const { return config->frame; }

    // client or server?
    const Mode& mode() const { return mode_; }
    bool is_server() const { return mode_.is_server(); }
    bool is_client() const { return mode_.is_client(); }

    // tcp/udp mode
    const bool is_tcp() { return config->protocol.is_tcp(); }
    const bool is_udp() { return config->protocol.is_udp(); }

    // configuration
    const Config& conf() const { return *config; }
    const Config::Ptr& conf_ptr() const { return config; }

    // stats
    SessionStats& stat() const { return *stats; }

  private:
    void reset_all()
    {
      if (primary)
	primary->rekey(CryptoDCInstance::DEACTIVATE_ALL);
      primary.reset();
      secondary.reset();
    }

    virtual void control_net_send(const Buffer& net_buf) = 0;

    virtual void control_recv(BufferPtr& app_bp) = 0;

    // Called on client to request username/password credentials.
    // Should be overriden by derived class if credentials are required.
    // username and password should be written into buf with write_auth_string().
    virtual void client_auth(Buffer& buf)
    {
      write_empty_string(buf); // username
      write_empty_string(buf); // password
    }

    // Called on server with credentials and peer info provided by client.
    // Should be overriden by derived class if credentials are required.
    virtual void server_auth(const std::string& username,
			     const SafeString& password,
			     const std::string& peer_info)
    {
    }

    // Called when initial KeyContext transitions to ACTIVE state
    virtual void active()
    {
    }

    void update_last_received()
    {
      keepalive_expire = *now_ + config->keepalive_timeout;
    }

    void net_send(const unsigned int key_id, const Packet& net_pkt)
    {
      control_net_send(net_pkt.buffer());
    }

    void app_recv(const unsigned int key_id, BufferPtr& to_app_buf)
    {
      control_recv(to_app_buf);
    }

    // we're getting a request from peer to renegotiate.
    bool renegotiate_request(Packet& pkt)
    {
      if (KeyContext::validate(pkt.buffer(), *this, now_))
	{
	  secondary.reset(new KeyContext(*this, false));
	  return true;
	}
      else
	return false;
    }

    // select a KeyContext (primary or secondary) for received network packets
    KeyContext& select_key_context(const PacketType& type, const bool control)
    {
      const unsigned int flags = type.flags & (PacketType::DEFINED|PacketType::SECONDARY|PacketType::CONTROL);
      if (!control)
	{
	  if (flags == (PacketType::DEFINED))
	    return *primary;
	  else if (flags == (PacketType::DEFINED|PacketType::SECONDARY) && secondary)
	    return *secondary;
	}
      else
	{
	  if (flags == (PacketType::DEFINED|PacketType::CONTROL))
	    return *primary;
	  else if (flags == (PacketType::DEFINED|PacketType::SECONDARY|PacketType::CONTROL) && secondary)
	    return *secondary;
	}
      throw select_key_context_error();
    }

    // Select a KeyContext (primary or secondary) for control channel sends.
    // NOTE: possible incompatibility with existing OpenVPN protocol.
    // Even after new key context goes active, we still wait for
    // KEV_BECOME_PRIMARY event before we use it for app-level control-channel
    // transmissions.  Simulations have found this method to be more reliable.
    // To revert to old OpenVPN behavior, call enable_fast_transition() method.
    KeyContext& select_control_send_context()
    {
      if (!fast_transition || !secondary)
	{
	  return *primary;
	}
      else
	{
	  const Time p = primary->reached_active();
	  const Time s = secondary->reached_active();
	  if (p.defined() && s.defined() && s > p)
	    return *secondary;
	  else
	    return *primary;
	}
    }

    // Possibly send a keepalive message, and check for expiration
    // of session due to lack of received packets from peer.
    void keepalive_housekeeping()
    {
      const Time now = *now_;

      // check for keepalive timeouts
      if (now >= keepalive_xmit)
	{
	  primary->send_keepalive();
	  update_last_sent();
	}
      if (now >= keepalive_expire)
	{
	  // no contact with peer, disconnect
	  stats->error(Error::KEEPALIVE_TIMEOUT);
	  disconnect(Error::KEEPALIVE_TIMEOUT);
	}
    }

    // Process KEV_x events
    // Return true if any events were processed.
    bool process_events()
    {
      bool did_work = false;

      // primary
      if (primary->event_pending())
	{
	  process_primary_event();
	  did_work = true;
	}

      // secondary
      if (secondary && secondary->event_pending())
	{
	  process_secondary_event();
	  did_work = true;
	}

      return did_work;
    }

    // Promote a newly renegotiated KeyContext to primary status.
    // This is usually triggered by become_primary variable (Time::Duration)
    // in Config.
    void promote_secondary_to_primary()
    {
      primary.swap(secondary);
      primary->rekey(CryptoDCInstance::PROMOTE_SECONDARY_TO_PRIMARY);
      secondary->prepare_expire();
      OPENVPN_LOG_PROTO_VERBOSE("*** PROMOTE_SECONDARY_TO_PRIMARY pri=" << primary->key_id() << " sec=" << secondary->key_id());
    }

    void process_primary_event()
    {
      const KeyContext::EventType ev = primary->get_event();
      if (ev != KeyContext::KEV_NONE)
	{
	  primary->reset_event();
	  switch (ev)
	    {
	    case KeyContext::KEV_ACTIVE:
	      OPENVPN_LOG_PROTO_VERBOSE("*** SESSION_ACTIVE");
	      primary->rekey(CryptoDCInstance::ACTIVATE_PRIMARY);
	      active();
	      break;
	    case KeyContext::KEV_RENEGOTIATE:
	      renegotiate();
	      break;
	    case KeyContext::KEV_EXPIRE:
	      if (secondary && !secondary->invalidated())
		promote_secondary_to_primary();
	      else
		{
		  stats->error(Error::PRIMARY_EXPIRE);
		  disconnect(Error::PRIMARY_EXPIRE); // primary context expired and no secondary context available
		}
	      break;
	    case KeyContext::KEV_NEGOTIATE_FAILED:
	      stats->error(Error::HANDSHAKE_TIMEOUT);
	      disconnect(Error::HANDSHAKE_TIMEOUT);   // primary negotiation failed
	      break;
	    default:
	      break;
	    }
	}
    }

    void process_secondary_event()
    {
      const KeyContext::EventType ev = secondary->get_event();
      if (ev != KeyContext::KEV_NONE)
	{
	  secondary->reset_event();
	  switch (ev)
	    {
	    case KeyContext::KEV_ACTIVE:
	      primary->prepare_expire();
	      break;
	    case KeyContext::KEV_BECOME_PRIMARY:
	      if (!secondary->invalidated())
		promote_secondary_to_primary();
	      break;
	    case KeyContext::KEV_EXPIRE:
	      secondary->rekey(CryptoDCInstance::DEACTIVATE_SECONDARY);
	      secondary.reset();
	      break;
	    case KeyContext::KEV_NEGOTIATE_FAILED:
	      stats->error(Error::HANDSHAKE_TIMEOUT);
	      renegotiate();
	      break;
	    default:
	      break;
	    }
	}
    }

    // key_id starts at 0, increments to KEY_ID_MASK, then recycles back to 1.
    // Therefore, if key_id is 0, it is the first key.
    unsigned int next_key_id()
    {
      ++n_key_ids;
      unsigned int ret = upcoming_key_id;
      if ((upcoming_key_id = (upcoming_key_id + 1) & KEY_ID_MASK) == 0)
	upcoming_key_id = 1;
      return ret;
    }

    // call whenever keepalive parms are modified,
    // to reset timers
    void keepalive_parms_modified()
    {
      update_last_received();

      // For keepalive_xmit timer, don't reschedule current cycle
      // unless it would fire earlier.  Subsequent cycles will
      // time according to new keepalive_ping value.
      const Time kx = *now_ + config->keepalive_ping;
      if (kx < keepalive_xmit)
	keepalive_xmit = kx;
    }

    // BEGIN ProtoContext data members

    Config::Ptr config;
    SessionStats::Ptr stats;

    size_t hmac_size;
    bool use_tls_auth;
    Mode mode_;                        // client or server
    unsigned int upcoming_key_id;
    unsigned int n_key_ids;

    TimePtr now_;                      // pointer to current time (a clone of config->now)
    Time keepalive_xmit;               // time in future when we will transmit a keepalive (subject to continuous change)
    Time keepalive_expire;             // time in future when we must have received a packet from peer or we will timeout session

    Time::Duration slowest_handshake_; // longest time to reach a successful handshake

    OvpnHMACInstance::Ptr ta_hmac_send;
    OvpnHMACInstance::Ptr ta_hmac_recv;
    PacketIDSend ta_pid_send;
    PacketIDReceive ta_pid_recv;

    ProtoSessionID psid_self;
    ProtoSessionID psid_peer;

    KeyContext::Ptr primary;
    KeyContext::Ptr secondary;
    bool dc_deferred;

    bool fast_transition;

    // END ProtoContext data members
  };

} // namespace openvpn

#endif //OPENVPN_SSL_PROTO_H
