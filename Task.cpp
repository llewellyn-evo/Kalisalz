//***************************************************************************
// Copyright 2007-2020 Universidade do Porto - Faculdade de Engenharia      *
// Laboratório de Sistemas e Tecnologia Subaquática (LSTS)                  *
//***************************************************************************
// This file is part of DUNE: Unified Navigation Environment.               *
//                                                                          *
// Commercial Licence Usage                                                 *
// Licencees holding valid commercial DUNE licences may use this file in    *
// accordance with the commercial licence agreement provided with the       *
// Software or, alternatively, in accordance with the terms contained in a  *
// written agreement between you and Faculdade de Engenharia da             *
// Universidade do Porto. For licensing terms, conditions, and further      *
// information contact lsts@fe.up.pt.                                       *
//                                                                          *
// Modified European Union Public Licence - EUPL v.1.1 Usage                *
// Alternatively, this file may be used under the terms of the Modified     *
// EUPL, Version 1.1 only (the "Licence"), appearing in the file LICENCE.md *
// included in the packaging of this file. You may not use this work        *
// except in compliance with the Licence. Unless required by applicable     *
// law or agreed to in writing, software distributed under the Licence is   *
// distributed on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS OF     *
// ANY KIND, either express or implied. See the Licence for the specific    *
// language governing permissions and limitations at                        *
// https://github.com/LSTS/dune/blob/master/LICENCE.md and                  *
// http://ec.europa.eu/idabc/eupl.html.                                     *
//***************************************************************************
// Author: Llewellyn-Fernandes                                              *
//***************************************************************************

// DUNE headers.
#include <DUNE/DUNE.hpp>
#include <DUNE/Utils/LineParser.hpp>
#include <vector>

namespace Transports
{
  //! Insert short task description here.
  //!
  //! Insert explanation on task behaviour here.
  //! @author Llewellyn-Fernandes
  namespace Kalisalz
  {
    using DUNE_NAMESPACES;
    struct Arguments
    {
      unsigned tcp_port;
      //! timer for TCP client async data
      double tcp_data_timer;
      //! maxcimum allowed clients
      unsigned int max_clients;
    };

    struct channel_info
    {
      std::string name;
      bool state;
    };

    struct Task: public DUNE::Tasks::Task
    {
      //! Constructor.
      //! @param[in] name task name.
      //! @param[in] ctx context.
      //! Arguments for the task
      Arguments m_args;
      // Socket handle.
      TCPSocket* m_sock;
      // I/O Multiplexer.
      Poll m_poll;
      // Clients.
      std::list<std::pair<TCPSocket* , DUNE::Utils::LineParser*>> m_clients;
      //! Timer to send data to clients
      DUNE::Time::Counter<double> m_client_data_timer;
      //! Temperature of Comm module
      double m_temperature;
      //! Pressure of Comm module
      double m_pressure;
      //! Relative Humidity inside Comm module
      double m_humidity;
      //! Array of channel info
      std::vector<channel_info> m_channels;


      Task(const std::string& name, Tasks::Context& ctx):
        DUNE::Tasks::Task(name, ctx),
        m_sock(NULL)
      {
        param("TCP - Port", m_args.tcp_port)
        .defaultValue("10000")
        .description("TCP port to listen on");

        param("TCP Data Timer", m_args.tcp_data_timer)
        .defaultValue("5.0")
        .description("Time between async TCP data");

        param("Maximum Clients", m_args.max_clients)
        .defaultValue("5")
        .description("Maximum Number of clients allowed to connect at a time");

        bind<IMC::Temperature>(this);
        bind<IMC::Pressure>(this);
        bind<IMC::RelativeHumidity>(this);
        bind<IMC::PowerChannelState>(this);
        bind<IMC::SmsStatus>(this);
        bind<IMC::TextMessage>(this);
      }

      //! Update internal state with new parameter values.
      void
      onUpdateParameters(void)
      {
      }

      //! Reserve entity identifiers.
      void
      onEntityReservation(void)
      {
      }

      //! Resolve entity names.
      void
      onEntityResolution(void)
      {
      }

      //! Acquire resources.
      void
      onResourceAcquisition(void)
      {
        try
        {
          m_sock = new TCPSocket;
        }
        catch (std::runtime_error& e)
        {
          throw RestartNeeded(e.what(), 30);
        }
      }

      //! Initialize resources.
      void
      onResourceInitialization(void)
      {
        if (m_sock != NULL)
        {
          m_sock->bind(m_args.tcp_port);
          m_sock->listen(m_args.max_clients);
          m_sock->setNoDelay(true);
          m_poll.add(*m_sock);
          inf("Listening on 0.0.0.0:%d" , m_args.tcp_port);
          m_client_data_timer.setTop(m_args.tcp_data_timer);
        }
      }

      //! Release resources.
      void
      onResourceRelease(void)
      {
        if (m_sock != NULL)
        {
          m_poll.remove(*m_sock);
          delete m_sock;
          m_sock = NULL;

          std::list<std::pair<TCPSocket* , DUNE::Utils::LineParser*>>::iterator iter = m_clients.begin();
          while (iter != m_clients.end())
          {
            m_poll.remove(*(iter->first));
            delete iter->first;
            ++iter;
          }
          m_clients.clear();
        }
      }

      void
      checkMainSocket(void)
      {
        if (m_poll.wasTriggered(*m_sock))
        {
          inf(DTR("accepting connection request"));
          try
          {
            TCPSocket* nc = m_sock->accept();
            DUNE::Utils::LineParser* parser = new DUNE::Utils::LineParser("\r\n");
            m_clients.push_back(std::make_pair(nc , parser));
            m_poll.add(*nc);
          }
          catch (std::runtime_error& e)
          {
            err("%s", e.what());
          }
        }
      }

      void
      consume(const IMC::TextMessage* msg)
      {
        std::string resp = "+SMSRECV," + msg->origin + "," + msg->text + "\r\n";
        dispatchToClients(resp);
      }

      void
      consume(const IMC::SmsStatus* msg)
      {
        std::stringstream resp;
        resp  << "+SMSSTATE," << msg->req_id << "," << msg->status;
        if (!msg->info.empty())
          resp << "," << msg->info << "\r\n";
        else
          resp << "\r\n";

        dispatchToClients(resp.str());
      }

      void
      consume(const IMC::Temperature* msg)
      {
        m_temperature = msg->value;
      }

      void
      consume(const IMC::Pressure* msg)
      {
        m_pressure = msg->value;
      }

      void
      consume(const IMC::RelativeHumidity* msg)
      {
        m_humidity = msg->value;
      }

      void
      consume(const IMC::PowerChannelState* msg)
      {
        for (uint8_t i = 0 ; i < m_channels.size() ; i++)
        {
          if (m_channels[i].name == msg->name)
          {
            m_channels[i].state = msg->state;
            return;
          }
        }
        channel_info channel;
        channel.name = msg->name;
        channel.state = msg->state;
        m_channels.push_back(channel);
      }

      void
      checkClientSockets(void)
      {
        std::list<std::pair<TCPSocket* , DUNE::Utils::LineParser*>>::iterator iter = m_clients.begin();
        while (iter != m_clients.end())
        {
          if (m_poll.wasTriggered(*(iter->first)))
          {
            try
            {
              char bfr[512];
              int rv = (iter->first)->read(bfr, sizeof(bfr));
              if (rv)
              {
                (iter->second)->append(bfr , rv);

                std::vector<std::string> lines;
                if ((iter->second)->parse(lines))
                {
                  for (unsigned int i = 0 ; i < lines.size() ; i ++)
                  {
                    std::string response = "ERROR\r\n";
                    if (lines[i].find("$PCONTROL,") != std::string::npos)
                    {
                      std::vector<std::string> parts;
                      String::split(lines[i], ",", parts);
                      if (parts.size() > 2)
                      {
                        //Check If PCC name in vector
                        for (unsigned int j = 0 ; j < m_channels.size() ; j++ )
                        {
                          if (m_channels[j].name == parts[1])
                          {
                            IMC::PowerChannelControl pcc;
                            pcc.name = parts[1];
                            pcc.op = std::stoi(parts[2]);
                            dispatch(pcc);
                            response.clear();
                            response = "OK\r\n";
                          }
                        }
                      }
                    }
                    else if (lines[i].find("$SMSSEND,") != std::string::npos)
                    {
                      std::vector<std::string> parts;
                      String::split(lines[i], ",", parts);
                      if (parts.size() > 4)
                      {
                        IMC::SmsRequest msg;
                        msg.req_id = std::stoi(parts[1]);
                        msg.destination = parts[2];
                        msg.sms_text = parts[3];
                        msg.timeout = std::stoi(parts[4]);
                        dispatch(msg);
                        response.clear();
                        response = "OK\r\n";
                      }
                    }
                    (iter->first)->writeString(response.c_str());
                  }
                }
              }
            }
            catch (Network::ConnectionClosed& e)
            {
              (void)e;
              m_poll.remove((*iter->first));
              iter = m_clients.erase(iter);
              continue;
            }
            catch (std::runtime_error& e)
            {
              err("%s", e.what());
            }
          }
          ++iter;
        }
      }

      //! Dispatch data to all TCP clients
      void
      dispatchToClients(const std::string str)
      {
        std::list<std::pair<TCPSocket* , DUNE::Utils::LineParser*>>::iterator iter = m_clients.begin();
        while (iter != std::end(m_clients))
        {
          try
          {
            (iter->first)->writeString(str.c_str());
          }
          catch (std::runtime_error& e)
          {
            err("%s", e.what());
            m_poll.remove(*(iter->first));
            iter = m_clients.erase(iter);
          }
          ++iter;
        }
      }


      //! Main loop.
      void
      onMain(void)
      {
        while (!stopping())
        {
          if (m_poll.poll(0.005))
          {
            checkMainSocket();
            checkClientSockets();
          }

          if (m_client_data_timer.overflow())
          {
            std::stringstream data;
            data << "+TPH," << std::fixed << std::setprecision(2) << m_temperature << "," << m_pressure << "," << m_humidity << "\r\n";
            dispatchToClients(data.str());
            m_client_data_timer.reset();
          }
          waitForMessages(0.005);
        }
      }
    };
  }
}

DUNE_TASK
