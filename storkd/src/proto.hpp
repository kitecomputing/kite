#ifndef __stork_proto_HPP__
#define __stork_proto_HPP__

#include <cstdint>
#include <vector>
#include <sstream>
#include <iostream>
#include <limits>

#include <boost/beast.hpp>
#include <boost/log/trivial.hpp>
#include <boost/array.hpp>
#include <boost/optional.hpp>
#include <boost/asio.hpp>

namespace stork {
  namespace proto {
    class ProtoParseException : public std::exception {
    public:
      inline ProtoParseException(const char *what)
        : m_what(what) { };
      virtual ~ProtoParseException();

      virtual const char* what() const noexcept;
    private:
      const char* m_what;
    };

    class ProtoParser {
    public:
      inline ProtoParser(std::istream &stream)
        : m_stream(stream) {
      };

      template<typename T>
      ProtoParser &parse(const char *what, T& out, bool network = true) {
        if ( m_stream.read((char *) &out, sizeof(T)) ) {
#ifdef LITTLE_ENDIAN
          if ( network )
            std::reverse((char *) &out, ((char *) &out) + sizeof(T));
#endif
          return *this;
        } else
          throw ProtoParseException(what);
      }

      template<typename T, typename Handler>
      ProtoParser &parseList(const char *what, T out, Handler on_item) {
        std::uint32_t len;
        try {
          parse("list length", len);
        } catch ( ProtoParseException &e ) {
          throw ProtoParseException(what);
        };

        for ( std::uint32_t i = 0; i < len; ++i ) {
          *out = on_item();
          out ++;
        }

        return *this;
      }

      template<typename T>
      ProtoParser &parseObject(const char *what, T& out) {
        out.parse_proto(*this);
        return *this;
      }

      ProtoParser &parseFixedLenString(const char *what, std::string &out, std::size_t len);
      ProtoParser &parseVarLenString(const char *what, std::string &out);

      template<typename Handler>
      inline ProtoParser &parseOptional(const char *what, Handler on_has) {
        std::uint8_t has_optional;
        parse(what, has_optional);

        if ( has_optional == 0 )
          return *this;
        else {
          on_has();
          return *this;
        }
      }

    private:
      std::istream &m_stream;
    };

    class ProtoBuilder {
    public:
      inline ProtoBuilder(std::ostream &stream)
        : m_stream(stream) {
      }

      template<typename T>
      ProtoBuilder &inter(const T& out, bool network=true) {
#ifdef LITTLE_ENDIAN
        char buf[sizeof(T)];
        std::copy((const char*) &out,
                  ((const char*) &out) + sizeof(T),
                  buf);
        if ( network )
          std::reverse(buf, buf + sizeof(T));
#else
        const char *buf((const char*) &out);
#endif

        m_stream.write((const char*) buf, sizeof(T));
        return *this;
      }

      template<typename T>
      ProtoBuilder &interObject(const T& out) {
        out.build_proto(*this);
        return *this;
      }

      template<typename T, typename Handler>
      ProtoBuilder &interList(const T& l,Handler h) {
        std::uint32_t l_len(l.size());

        inter(l_len);

        for ( auto i: l ) {
          h(i);
        }

        return *this;
      }

      ProtoBuilder &interVarLenString(const std::string &s);
      ProtoBuilder &interFixedLenString(const std::string &s, std::size_t l, char fill='\0');

      template<typename T>
      ProtoBuilder &interOptional(const boost::optional<T> &out,
                                  std::function<void(const T&)> onValue) {
        std::uint8_t optional_present = out ? 0xFF : 0x00;
        inter<std::uint8_t>(optional_present);
        if ( out )
          onValue(*out);

        return *this;
      }

    private:
      std::ostream &m_stream;
    };

    class StreamSocket; // TCP, Unix
    class SeqPacketSocket; // SCTP, WebSockets

    template<typename T>
    class ProtocolProperties {
    };

    template<>
    class ProtocolProperties<boost::asio::ip::tcp> {
    public:
      using framing_type = StreamSocket;
      static constexpr bool uses_dynamic_buffers = false;
    };

    template<>
    class ProtocolProperties<boost::asio::local::stream_protocol> {
    public:
      using framing_type = StreamSocket;
      static constexpr bool uses_dynamic_buffers = false;
    };

    template<typename Size, typename Socket, typename FramingType>
    class FrameWriter {
    public:
      static void async_write_frame(Socket &socket, Size max_sz, const std::string &buf, Size &cur_size,
                                    std::function<void(boost::system::error_code)> cb);
    };

    template<typename Size, typename Socket>
    class FrameWriter<Size, Socket, SeqPacketSocket> {
    public:
      static void async_write_frame(Socket &socket, Size max_sz,
                                    const std::string &buf, Size &cur_size,
                                    std::function<void(boost::system::error_code)> cb) {

        socket.async_write(boost::asio::buffer(buf),
                           [cb](boost::system::error_code ec,
                                std::size_t bytes_sent) {
                             cb(ec);
                           });
      }

      static std::size_t write_frame(Socket &socket, Size max_sz,
                                     const std::string &buf, Size &cur_size) {
        return socket.write(boost::asio::buffer(buf));
      }
    };

    template<typename Size, typename Socket>
    class FrameWriter<Size, Socket, StreamSocket> {
    public:
      static void async_write_frame(Socket &socket, Size max_sz,
                                    const std::string &buf, Size& cur_size,
                                    std::function<void(boost::system::error_code)> cb) {

#ifdef LITTLE_ENDIAN
        std::reverse((char *)&cur_size,
                     ((char *)&cur_size) + sizeof(Size));
#endif

        boost::array<boost::asio::const_buffer, 2> bufs = {
          boost::asio::buffer((const char*) &cur_size, sizeof(Size)),
          boost::asio::buffer(buf)
        };

        boost::asio::async_write(socket, bufs, [cb](boost::system::error_code ec, std::size_t len) {
            cb(ec);
          });
      }

      static std::size_t write_frame(Socket &socket, Size max_sz,
                                     const std::string &buf, Size &cur_size) {

#ifdef LITTLE_ENDIAN
        std::reverse((char *)&cur_size,
                     ((char *)&cur_size) + sizeof(Size));
#endif
        boost::array<boost::asio::const_buffer, 2> bufs = {
          boost::asio::buffer((const char*) &cur_size, sizeof(Size)),
          boost::asio::buffer(buf)
        };

        return boost::asio::write(socket, bufs);
      }
    };

    // TODO convert this into templated class
    template<typename Size, typename Socket>
    class FramedSender {
    public:
      FramedSender(Socket &socket, Size max_frame_size = 0)
        : m_socket(socket), m_max_frame_size(max_frame_size) {
      }

      inline operator bool() const {
        if ( m_max_frame_size ) {
          return m_buffer.str() < m_max_frame_size;
        } else
          return true;
      }

      template<typename Message>
      void async_write(const Message &msg,
                       std::function<void(boost::system::error_code)> cb) {
        ProtoBuilder builder(m_buffer);
        msg.write(builder);

        m_cur_frame = m_buffer.str();
        m_buffer.str("");

        m_cur_frame_size = m_cur_frame.size();
        if ( m_max_frame_size != 0 && m_cur_frame_size > m_max_frame_size )
          throw std::length_error("async_write: frame is larger than maximum size");

        FrameWriter< Size, Socket, typename ProtocolProperties<typename Socket::protocol_type>::framing_type >::async_write_frame(m_socket, m_max_frame_size, m_cur_frame, m_cur_frame_size, cb);
      }

      template<typename Message>
      std::size_t write(const Message &msg) {
        ProtoBuilder builder(m_buffer);
        msg.write(builder);

        m_cur_frame = m_buffer.str();
        m_buffer.str("");

        m_cur_frame_size = m_cur_frame.size();
        if ( m_max_frame_size != 0 && m_cur_frame_size > m_max_frame_size )
          throw std::length_error("write: frame is larger than maximum size");

        return FrameWriter< Size, Socket, typename ProtocolProperties<typename Socket::protocol_type>::framing_type >::write_frame(m_socket, m_max_frame_size, m_cur_frame, m_cur_frame_size);
      }

    private:
      Socket &m_socket;
      Size m_max_frame_size, m_cur_frame_size;
      std::string m_cur_frame;

      std::stringstream m_buffer; // TODO use a fixed size buffer
    };

    template<typename Size, typename Socket, typename FramingType, bool DynamicBuffers>
    class FrameReader {
    public:
      static void async_read_frame(Socket &socket, Size max_size, std::string &buf,
                                   Size &cur_size, std::function<void(boost::system::error_code)> cb);
    };

    template<typename Size, typename Socket>
    class FrameReader<Size, Socket, SeqPacketSocket, true> {
    public:
      static void async_read_frame(Socket &socket, Size max_size,
                                   std::string &buf, Size &cur_size,
                                   std::function<void(boost::system::error_code)> cb) {

        if ( max_size == 0 ) {
          max_size = std::numeric_limits<Size>::max();
          BOOST_LOG_TRIVIAL(warning) << "Reading from seq packet socket with no maximum frame size";
        }

        std::shared_ptr<boost::beast::flat_buffer> dyn_buffer(std::make_shared<boost::beast::flat_buffer>(max_size));

        socket.async_read
          (*dyn_buffer,
           [dyn_buffer, &cur_size, cb, &buf]
           (boost::system::error_code ec,
            std::size_t bytes_sent) {

            if ( ec ) {
              BOOST_LOG_TRIVIAL(error) << "Discarded packet that was too long";
              cb(ec);
            } else {
              auto buf_data(dyn_buffer->data());
              auto buf_data_raw(boost::asio::buffer_cast<char *>(buf_data));
              auto buf_size(boost::asio::buffer_size(buf_data));

              buf.assign(buf_data_raw, buf_data_raw + buf_size);

              cur_size = bytes_sent;
              cb(ec);
            }
          });
      }
    };

    template<typename Size, typename Socket>
    class FrameReader<Size, Socket, StreamSocket, false> {
    public:
      static void async_read_frame(Socket &socket, Size max_size,
                                   std::string &buf, Size &cur_size,
                                   std::function<void(boost::system::error_code)> cb) {
        boost::asio::async_read
          (socket, boost::asio::buffer((char *)&cur_size, sizeof(Size)),
           [max_size, &cur_size, &buf, &socket, cb{std::move(cb)}](boost::system::error_code ec, std::size_t bytes_read) {
            if ( ec ) {
              buf.resize(0);
              cb(ec);
            } else {
              if ( bytes_read < sizeof(Size) ) {
                buf.resize(0);
                cb(boost::system::errc::make_error_code(boost::system::errc::broken_pipe));
              } else {
#ifdef LITTLE_ENDIAN
                std::reverse((char *)&cur_size,
                             ((char *)&cur_size) + sizeof(Size));
#endif

                if ( max_size != 0 && cur_size > max_size ) {
                  buf.resize(0);
                  cb(boost::system::errc::make_error_code(boost::system::errc::message_size));
                } else {
                  buf.resize(cur_size);
                  boost::asio::async_read
                    (socket, boost::asio::buffer(buf),
                     [&buf, &cur_size, cb{std::move(cb)}](boost::system::error_code ec, std::size_t read) {
                      if ( ec ) {
                        buf.resize(0);
                        cb(ec);
                      } else if ( read < cur_size ) {
                        buf.resize(0);
                        cb(boost::system::errc::make_error_code(boost::system::errc::broken_pipe));
                      } else {
                        cb(ec);
                      }
                    });
                }
              }
            }
          });
      }
    };

    template<typename T, typename Socket>
    class FramedReader {
    public:
      FramedReader(Socket &socket, T max_frame_size = 0)
        : m_socket(socket), m_max_frame_size(max_frame_size) {
      }

      void async_read_frame(std::function<void(boost::system::error_code)> cb) {
        FrameReader< T, Socket, typename ProtocolProperties<typename Socket::protocol_type>::framing_type,
                     ProtocolProperties<typename Socket::protocol_type>::uses_dynamic_buffers >
          ::async_read_frame(m_socket, m_max_frame_size, m_buffer, m_cur_frame_size, cb);
      }

      const std::string &cur_frame() const { return m_buffer; }

      template<typename Read>
      Read read() const {
        std::stringstream resp(m_buffer);
        ProtoParser p(resp);
        return Read(p);
      }

    private:
      Socket &m_socket;
      T m_cur_frame_size, m_max_frame_size;
      std::string m_buffer;
    };
  }
}

#endif
