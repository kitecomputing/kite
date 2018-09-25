#ifndef __stork_flock_manager_HPP__
#define __stork_flock_manager_HPP__

#include <boost/asio.hpp>

#include "configuration.hpp"
#include "registry.hpp"

namespace stork {
  namespace flock {
    namespace flockd {
      class IFlockServer;

      // Implementation of RFC5389(https://tools.ietf.org/html/rfc5389)
      class StunServer {
      public:
        StunServer(boost::asio::io_service &io_svc, std::uint16_t stun_port);

       private:
        boost::asio::io_service &m_io_service;
        std::uint16_t m_stun_port;

        boost::asio::ip::tcp::acceptor m_tcp_acceptor;
        boost::asio::ip::udp::socket m_udp_socket;
      };

      class Manager {
      public:
        Manager(boost::asio::io_service &svc, const Configuration &conf);

        inline boost::asio::io_service &service() { return m_io_service; };
        inline const Configuration &config() const { return m_config; }
        inline ApplianceRegistry &registry() { return m_registry; }
        inline const ApplianceRegistry &registry() const { return m_registry; }

        int run();

      private:
        boost::asio::io_service &m_io_service;
        const Configuration &m_config;
        ApplianceRegistry m_registry;

        StunServer m_stun_server;

        std::list< std::shared_ptr<IFlockServer> > m_servers;
      };
    }
  }
}

#endif
