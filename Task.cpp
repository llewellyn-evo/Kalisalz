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
      std::list<TCPSocket*> m_clients;
      //! Timer to send data to clients
      DUNE::Time::Counter<float> m_client_data_timer;
      //! Temperature of Comm module
      double m_temperature;
      //! Pressure of Comm module
      double m_pressure;
      //! Relative Humidity inside Comm module
      double m_humidity;
      //! count to send Channel states
      uint8_t m_send_channel_state;


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
          m_sock->listen(1024);
          m_sock->setNoDelay(true);
          m_poll.add(*m_sock);
          this->inf("Listening on 0.0.0.0:%d" , m_args.tcp_port);
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


          std::list<TCPSocket*>::iterator itr = m_clients.begin();
          for (; itr != m_clients.end(); ++itr)
          {
            m_poll.remove(*(*itr));
            delete *itr;
          }
          m_clients.clear();
        }
      }

      void
      checkMainSocket(void)
      {
        if (m_poll.wasTriggered(*m_sock))
        {
          this->inf(DTR("accepting connection request"));
          try
          {
            TCPSocket* nc = m_sock->accept();
            m_clients.push_back(nc);
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
        char state[200];
        int len = sprintf(state,"+SMSRECV,%s,%s\r\n" , msg->origin.c_str() , msg->text.c_str() );
        dispatchToClients(state , len);
      }

      void
      consume(const IMC::SmsStatus* msg)
      {
        char state[200];
        int len;
        if (!msg->info.empty())
          len = sprintf(state,"+SMSSTATE,%d,%d,%s\r\n" ,msg->req_id , msg->status , msg->info.c_str());
        else
          len = sprintf(state,"+SMSSTATE,%d,%d\r\n" ,msg->req_id , msg->status);

        dispatchToClients(state , len);
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
        char state[50];
        if (m_send_channel_state)
        {
          int len = sprintf(state,"+PSTATE,%s,%d\r\n" , msg->name.c_str() , msg->state);
          dispatchToClients(state , len);
          m_send_channel_state++;
        }
      }

      void
      checkClientSockets(void)
      {
        char bfr[512];
        char command_resp[10] = "ERROR\r\n";

        std::list<TCPSocket*>::iterator itr = m_clients.begin();
        while (itr != m_clients.end())
        {
          if (m_poll.wasTriggered(*(*itr)))
          {
            try
            {
              int rv = (*itr)->read(bfr, sizeof(bfr));

              if (rv > 0 && !strncmp(bfr , "$PCONTROL," , 10))
              {
                std::vector<std::string> parts;
                String::split(std::string(bfr), ",", parts);
                if (parts.size() > 2)
                {
                  IMC::PowerChannelControl pcc;
                  pcc.name = parts[1];
                  pcc.op = std::stoi(parts[2]);
                  dispatch(pcc);
                  memset(command_resp , 0 , sizeof(command_resp));
                  sprintf(command_resp , "OK\r\n");
                }
              }
              else if (rv > 0  && !strncmp(bfr , "$SMSSEND," , 5))
              {
                std::vector<std::string> parts;
                String::split(std::string(bfr), ",", parts);

                if (parts.size() > 4)
                {
                  IMC::SmsRequest msg;
                  msg.req_id = std::stoi(parts[1]);
                  msg.destination = parts[2];
                  msg.sms_text = parts[3];
                  msg.timeout = std::stoi(parts[4]);
                  dispatch(msg);
                  memset(command_resp , 0 , sizeof(command_resp));
                  sprintf(command_resp , "OK\r\n");
                }
              }
              (*itr)->write(command_resp, (unsigned)strlen(command_resp));
              //! Return Value Here
            }
            catch (Network::ConnectionClosed& e)
            {
              (void)e;
              m_poll.remove(*(*itr));
              delete *itr;
              itr = m_clients.erase(itr);
              continue;
            }
            catch (std::runtime_error& e)
            {
              err("%s", e.what());
            }
          }
          ++itr;
        }
      }

      //! Dispatch data to all TCP clients
      void
      dispatchToClients(const char* bfr, unsigned bfr_len)
      {
        std::list<TCPSocket*>::iterator itr = m_clients.begin();
        while (itr != m_clients.end())
        {
          try
          {
            (*itr)->write(bfr, bfr_len);
            ++itr;
          }
          catch (std::runtime_error& e)
          {
            err("%s", e.what());
            m_poll.remove(*(*itr));
            delete *itr;
            itr = m_clients.erase(itr);
          }
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

          if (m_send_channel_state == 6)
          {
            m_send_channel_state = 0;
          }

          if (m_client_data_timer.overflow())
          {
            char data[100];
            int len = sprintf(data , "+TPH,%2.2f,%2.2f,%2.2f\r\n",m_temperature , m_pressure , m_humidity);
            dispatchToClients(data , len);
            m_send_channel_state = 1;
            m_client_data_timer.reset();
          }
          waitForMessages(0.005);
        }
      }
    };
  }
}

DUNE_TASK
