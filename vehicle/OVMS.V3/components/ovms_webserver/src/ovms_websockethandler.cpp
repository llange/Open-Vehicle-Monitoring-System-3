/*
;    Project:       Open Vehicle Monitor System
;    Date:          14th March 2017
;
;    Changes:
;    1.0  Initial release
;
;    (C) 2018       Michael Balzer
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in
; all copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
; THE SOFTWARE.
*/

// We're using ESP_EARLY_LOG* (direct USB console output) for protocol debug logging.
// To enable protocol debug logging locally, uncomment:
// #define LOG_LOCAL_LEVEL ESP_LOG_VERBOSE

#include "ovms_log.h"
static const char *TAG = "websocket";

#include <string.h>
#include <stdio.h>
#include "ovms_webserver.h"
#include "ovms_config.h"
#include "ovms_metrics.h"
#include "ovms_boot.h"
#include "metrics_standard.h"
#include "buffered_shell.h"
#include "vehicle.h"


/**
 * WebSocketHandler transmits JSON data in chunks to the WebSocket client
 *  and serializes transmits initiated from all contexts.
 * 
 * On creation it will do a full update of all metrics.
 * Later on it receives TX jobs through the queue.
 * 
 * Job processing & data transmission is protected by the mutex against
 * parallel execution. TX init is done either by the mongoose EventHandler
 * on connect/poll or by the UpdateTicker. The EventHandler triggers immediate
 * successive sends, the UpdateTicker sends collected intermediate updates.
 */

WebSocketHandler::WebSocketHandler(mg_connection* nc, size_t slot, size_t modifier, size_t reader)
  : MgHandler(nc)
{
  ESP_LOGV(TAG, "WebSocketHandler[%p] init: handler=%p modifier=%d", nc, this, modifier);
  
  m_slot = slot;
  m_modifier = modifier;
  m_reader = reader;
  m_jobqueue = xQueueCreate(50, sizeof(WebSocketTxJob));
  m_jobqueue_overflow_status = 0;
  m_jobqueue_overflow_logged = 0;
  m_jobqueue_overflow_dropcnt = 0;
  m_jobqueue_overflow_dropcntref = 0;
  m_job.type = WSTX_None;
  m_sent = m_ack = m_last = 0;
  m_units_subscribed = false;
  m_units_prefs_subscribed = false;

  MyMetrics.InitialiseSlot(m_slot);
  MyUnitConfig.InitialiseSlot(m_slot);
  
  // Register as logging console:
  SetMonitoring(true);
  MyCommandApp.RegisterConsole(this);
}

WebSocketHandler::~WebSocketHandler()
{
  MyCommandApp.DeregisterConsole(this);
  if (m_jobqueue) {
    while (xQueueReceive(m_jobqueue, &m_job, 0) == pdTRUE)
      ClearTxJob(m_job);
    vQueueDelete(m_jobqueue);
  }
}


void WebSocketHandler::ProcessTxJob()
{
  ESP_EARLY_LOGV(TAG, "WebSocketHandler[%p]: ProcessTxJob type=%d, sent=%d ack=%d", m_nc, m_job.type, m_sent, m_ack);
  
  // process job, send next chunk:
  switch (m_job.type)
  {
    case WSTX_Event:
    {
      if (m_sent && m_ack) {
        ESP_EARLY_LOGV(TAG, "WebSocketHandler[%p]: ProcessTxJob type=%d done", m_nc, m_job.type);
        ClearTxJob(m_job);
      } else {
        std::string msg;
        msg.reserve(128);
        msg = "{\"event\":\"";
        msg += m_job.event;
        msg += "\"}";
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
        mg_ws_send(m_nc, msg.data(), msg.size(), WEBSOCKET_OP_TEXT);
#else /* MG_VERSION_NUMBER */
        mg_send_websocket_frame(m_nc, WEBSOCKET_OP_TEXT, msg.data(), msg.size());
#endif /* MG_VERSION_NUMBER */
        m_sent = 1;
      }
      break;
    }
    
    case WSTX_MetricsAll:
    case WSTX_MetricsUpdate:
    {
      // Note: this loops over the metrics by index, keeping the last checked position
      //  in m_last. It will not detect new metrics added between polls if they are
      //  inserted before m_last, so new metrics may not be sent until first changed.
      //  The Metrics set normally is static, so this should be no problem.
      
      // find start:
      int i;
      OvmsMetric* m;
      for (i=0, m=MyMetrics.m_first; i < m_last && m != NULL; m=m->m_next, i++);
      
      // build msg:
      if (m) {
        std::string msg;
        msg.reserve(2*XFER_CHUNK_SIZE+128);
        msg = "{\"metrics\":{";
        for (i=0; m && msg.size() < XFER_CHUNK_SIZE; m=m->m_next) {
          ++m_last;
          if (m->IsModifiedAndClear(m_modifier) || m_job.type == WSTX_MetricsAll) {
            if (i) msg += ',';
            msg += '\"';
            msg += m->m_name;
            msg += "\":";
            msg += m->AsJSON();
            i++;
          }
        }

        // send msg:
        if (i) {
          msg += "}}";
          ESP_EARLY_LOGV(TAG, "WebSocket msg: %s", msg.c_str());
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
          mg_ws_send(m_nc, msg.data(), msg.size(), WEBSOCKET_OP_TEXT);
#else /* MG_VERSION_NUMBER */
          mg_send_websocket_frame(m_nc, WEBSOCKET_OP_TEXT, msg.data(), msg.size());
#endif /* MG_VERSION_NUMBER */
          m_sent += i;
        }
      }

      // done?
      if (!m && m_ack == m_sent) {
        if (m_sent)
          ESP_EARLY_LOGV(TAG, "WebSocketHandler[%p]: ProcessTxJob type=%d done, sent=%d metrics", m_nc, m_job.type, m_sent);
        ClearTxJob(m_job);
      }
      
      break;
    }

    case WSTX_UnitMetricUpdate:
    {
      // Note: this loops over the metrics by index, keeping the last checked position
      //  in m_last. It will not detect new metrics added between polls if they are
      //  inserted before m_last, so new metrics may not be sent until first changed.
      //  The Metrics set normally is static, so this should be no problem.

      ESP_EARLY_LOGD(TAG, "WebSocketHandler[%p/%d]: ProcessTxJob MetricsUnitUpdate, last=%d sent=%d ack=%d", m_nc, m_modifier, m_last, m_sent, m_ack);
      // find start:
      int i;
      OvmsMetric* m;
      for (i=0, m=MyMetrics.m_first; i < m_last && m != NULL; m=m->m_next, i++);
      ESP_EARLY_LOGD(TAG, "WebSocketHandler[%p/%d]: ProcessTxJob MetricsUnitUpdate, i=%d", m_nc, m_modifier, i);
      if (m) { // Bypass this if we are on the 'just sent' leg.
        // build msg:
        std::string msg;
        msg.reserve(2*XFER_CHUNK_SIZE+128);
        msg = "{\"units\":{\"metrics\":{";

        // Cache the user mappings for each group.
        for (i=0; m && msg.size() < XFER_CHUNK_SIZE; m=m->m_next) {
          ++m_last;
          bool send = m->IsUnitSendAndClear(m_modifier);
          if (send) {
            if (i)
              msg += ',';
            metric_unit_t units = m->m_units;
            metric_unit_t user_units = MyUnitConfig.GetUserUnit(units);
            if (user_units ==  UnitNotFound)
              user_units = Native;
            std::string unitlabel = OvmsMetricUnitLabel((user_units == Native) ? units : user_units);

            const char *metricname = (units == Native) ? "Other" : OvmsMetricUnitName(units);
            if (metricname == NULL)
              metricname = "";
            const char *user_metricname = (user_units == Native) ? metricname : OvmsMetricUnitName(user_units);
            if (user_metricname == NULL)
              user_metricname = metricname;

            std::string entry = string_format("\"%s\":{\"native\":\"%s\",\"code\":\"%s\",\"label\":\"%s\"}",
               m->m_name, metricname, user_metricname, json_encode(unitlabel).c_str()
               );
            msg += entry;
            i++;
          }
        }

        // send msg:
        if (i) {
          msg += "}}}";
          ESP_EARLY_LOGD(TAG, "WebSocket msg: %s", msg.c_str());
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
          mg_ws_send(m_nc, msg.data(), msg.size(), WEBSOCKET_OP_TEXT);
#else /* MG_VERSION_NUMBER */
          mg_send_websocket_frame(m_nc, WEBSOCKET_OP_TEXT, msg.data(), msg.size());
#endif /* MG_VERSION_NUMBER */
          m_sent += i;
        }
      }

      // done?
      if (!m && m_ack == m_sent) {
        if (m_sent)
          ESP_EARLY_LOGD(TAG, "WebSocketHandler[%p/%d]: ProcessTxJob MetricsUnitsUpdate done, sent=%d metrics", m_nc, m_modifier, m_sent);
        ClearTxJob(m_job);
      }
      break;
    }
    case WSTX_UnitPrefsUpdate:
    {
      // Note: this loops over the metrics by index, keeping the last checked position
      //  in m_last. It will not detect new metrics added between polls if they are
      //  inserted before m_last, so new metrics may not be sent until first changed.
      //  The Metrics set normally is static, so this should be no problem.

      ESP_EARLY_LOGD(TAG, "WebSocketHandler[%p/%d]: ProcessTxJob MetricsVehicleUpdate, last=%d sent=%d ack=%d", m_nc, m_modifier, m_last, m_sent, m_ack);
      if (m_last < MyUnitConfig.config_groups.size()) {
        // Bypass this if we are on the 'just sent' leg.
        // build msg:
        std::string msg;
        msg.reserve(2*XFER_CHUNK_SIZE+128);
        msg = "{\"units\":{\"prefs\":{";

        // Cache the user mappings for each group.
        int i = 0;
        for (int groupindex = m_last;
             groupindex < MyUnitConfig.config_groups.size() && msg.size() < XFER_CHUNK_SIZE;
             ++groupindex) {
          ++m_last;
          metric_group_t group = MyUnitConfig.config_groups[groupindex];

          bool send = MyUnitConfig.IsModifiedAndClear(group, m_modifier);
          if (send) {
            metric_unit_t user_units = MyUnitConfig.GetUserUnit(group);
            std::string unitLabel;
            if (user_units == UnitNotFound)
              unitLabel = "null";
            else {

              unitLabel = '"';
              unitLabel += json_encode(std::string(OvmsMetricUnitLabel(user_units)));
              unitLabel += '"';
            }
            const char *groupName = OvmsMetricGroupName(group);
            const char *unitName = (user_units == Native) ? "Native" : OvmsMetricUnitName(user_units);
            std::string entry = string_format("%s\"%s\":{\"unit\":\"%s\",\"label\":%s}",
               i ? "," : "",
               groupName, unitName, unitLabel.c_str()
               );
            msg += entry;
            i++;
          }
        }

        // send msg:
        if (i) {
          msg += "}}}";
          ESP_EARLY_LOGD(TAG, "WebSocket msg: %s", msg.c_str());
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
          mg_ws_send(m_nc, msg.data(), msg.size(), WEBSOCKET_OP_TEXT);
#else /* MG_VERSION_NUMBER */
          mg_send_websocket_frame(m_nc, WEBSOCKET_OP_TEXT, msg.data(), msg.size());
#endif /* MG_VERSION_NUMBER */
          m_sent += i;
        }
      }

      // done?
      if (m_last >= MyUnitConfig.config_groups.size() && m_ack == m_sent) {
        if (m_sent)
          ESP_EARLY_LOGD(TAG, "WebSocketHandler[%p/%d]: ProcessTxJob MetricsUnitsUpdate done, sent=%d metrics", m_nc, m_modifier, m_sent);
        ClearTxJob(m_job);
      }
      break;
    }

    case WSTX_Notify:
    {
      if (m_sent && m_ack == m_job.notification->GetValueSize()+1) {
        // done:
        ESP_EARLY_LOGV(TAG, "WebSocketHandler[%p]: ProcessTxJob type=%d done, sent %d bytes", m_nc, m_job.type, m_sent);
        ClearTxJob(m_job);
      } else {
        // build frame:
        std::string msg;
        msg.reserve(XFER_CHUNK_SIZE+128);
        int op;
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
        bool should_clear_fin_bit = false;
        size_t buffer_start = 0;
#endif /* MG_VERSION_NUMBER */
        
        if (m_sent == 0) {
          op = WEBSOCKET_OP_TEXT;
          msg += "{\"notify\":{\"type\":\"";
          msg += m_job.notification->GetType()->m_name;
          msg += "\",\"subtype\":\"";
          msg += mqtt_topic(m_job.notification->GetSubType());
          msg += "\",\"value\":\"";
          m_sent = 1;
        } else {
          op = WEBSOCKET_OP_CONTINUE;
        }
        
        extram::string part = m_job.notification->GetValue().substr(m_sent-1, XFER_CHUNK_SIZE);
        msg += json_encode(part);
        m_sent += part.size();
        
        if (m_sent < m_job.notification->GetValueSize()+1) {
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
          should_clear_fin_bit = true;
          buffer_start = m_nc->send.len;
#else /* MG_VERSION_NUMBER */
          op |= WEBSOCKET_DONT_FIN;
#endif /* MG_VERSION_NUMBER */
        } else {
          msg += "\"}}";
        }
        
        // send frame:
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
        mg_ws_send(m_nc, msg.data(), msg.size(), op);
        if (should_clear_fin_bit) {
          m_nc->send.buf[buffer_start] &= ~128;  // Clear FIN flag
        }
#else /* MG_VERSION_NUMBER */
        mg_send_websocket_frame(m_nc, op, msg.data(), msg.size());
#endif /* MG_VERSION_NUMBER */
        ESP_EARLY_LOGV(TAG, "WebSocketHandler[%p]: ProcessTxJob type=%d: sent %d bytes, op=%04x", m_nc, m_job.type, m_sent, op);
      }
      break;
    }
    
    case WSTX_LogBuffers:
    {
      // Note: this sender loops over the buffered lines by index (kept in m_sent)
      // Single log lines may be longer than our nominal XFER_CHUNK_SIZE, but that is
      // very rarely the case, so we shouldn't need to additionally chunk them.
      LogBuffers* lb = m_job.logbuffers;

      // find next line:
      int i;
      LogBuffers::iterator it;
      for (i=0, it=lb->begin(); i < m_sent && it != lb->end(); it++, i++);
      
      if (it != lb->end()) {
        // encode & send:
        std::string msg;
        msg.reserve(strlen(*it)+128);
        msg = "{\"log\":\"";
        msg += json_encode(stripesc(*it));
        msg += "\"}";
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
        mg_ws_send(m_nc, msg.data(), msg.size(), WEBSOCKET_OP_TEXT);
#else /* MG_VERSION_NUMBER */
        mg_send_websocket_frame(m_nc, WEBSOCKET_OP_TEXT, msg.data(), msg.size());
#endif /* MG_VERSION_NUMBER */
        m_sent++;
      }
      else if (m_ack == m_sent) {
        // done:
        if (m_sent)
          ESP_EARLY_LOGV(TAG, "WebSocketHandler[%p]: ProcessTxJob type=%d done, sent=%d lines", m_nc, m_job.type, m_sent);
        ClearTxJob(m_job);
      }
      
      break;
    }
    
    case WSTX_Config:
    {
      // todo: implement
      ClearTxJob(m_job);
      m_sent = 0;
      break;
    }
    
    default:
      ClearTxJob(m_job);
      m_sent = 0;
      break;
  }
}


void WebSocketTxJob::clear(size_t client)
{
  auto& slot = MyWebServer.m_client_slots[client];
  switch (type) {
    case WSTX_Event:
      if (event)
        free(event);
      break;
    case WSTX_Notify:
      if (notification) {
        OvmsNotifyType* mt = notification->GetType();
        if (mt) mt->MarkRead(slot.reader, notification);
      }
      break;
    case WSTX_LogBuffers:
      if (logbuffers)
        logbuffers->release();
      break;
    default:
      break;
  }
  type = WSTX_None;
}

void WebSocketHandler::ClearTxJob(WebSocketTxJob &job)
{
  job.clear(m_slot);
}


bool WebSocketHandler::AddTxJob(WebSocketTxJob job, bool init_tx)
{
  if (!m_jobqueue) return false;
  if (xQueueSend(m_jobqueue, &job, 0) != pdTRUE) {
    m_jobqueue_overflow_status |= 1;
    m_jobqueue_overflow_dropcnt++;
    return false;
  }
  else {
    if (m_jobqueue_overflow_status & 1)
      m_jobqueue_overflow_status++;
    if (init_tx && uxQueueMessagesWaiting(m_jobqueue) == 1)
      RequestPoll();
    return true;
  }
}

bool WebSocketHandler::GetNextTxJob()
{
  if (!m_jobqueue) return false;
  if (xQueueReceive(m_jobqueue, &m_job, 0) == pdTRUE) {
    // init new job state:
    m_sent = m_ack = m_last = 0;
    return true;
  } else {
    return false;
  }
}


void WebSocketHandler::InitTx()
{
  if (m_job.type != WSTX_None)
    return;
  
  // begin next job if idle:
  while (m_job.type == WSTX_None) {
    if (!GetNextTxJob())
      break;
    ProcessTxJob();
  }
}

void WebSocketHandler::ContinueTx()
{
  m_ack = m_sent;
  
  do {
    // process current job:
    ProcessTxJob();
    // check next if done:
  } while (m_job.type == WSTX_None && GetNextTxJob());
}


int WebSocketHandler::HandleEvent(int ev, void* p)
{
  switch (ev)
  {
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
    case MG_EV_WS_MSG:
#else /* MG_VERSION_NUMBER */
    case MG_EV_WEBSOCKET_FRAME:
#endif /* MG_VERSION_NUMBER */
    {
      // websocket message received
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
      struct mg_ws_message* wm = (struct mg_ws_message*) p;
#else /* MG_VERSION_NUMBER */
      websocket_message* wm = (websocket_message*) p;
#endif /* MG_VERSION_NUMBER */
      std::string msg;
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
      msg.assign(wm->data.ptr, wm->data.len);
#else /* MG_VERSION_NUMBER */
      msg.assign((char*) wm->data, wm->size);
#endif /* MG_VERSION_NUMBER */
      HandleIncomingMsg(msg);
      break;
    }
    
    case MG_EV_POLL:
      ESP_EARLY_LOGV(TAG, "WebSocketHandler[%p] EV_POLL qlen=%d jobtype=%d sent=%d ack=%d", m_nc,
        m_jobqueue ? uxQueueMessagesWaiting(m_jobqueue) : -1, m_job.type, m_sent, m_ack);
      // Check for new transmission:
      InitTx();
      // Log queue overflows & resolves:
      if (m_jobqueue_overflow_status > m_jobqueue_overflow_logged) {
        m_jobqueue_overflow_logged = m_jobqueue_overflow_status;
        if (m_jobqueue_overflow_status & 1) {
          ESP_LOGW(TAG, "WebSocketHandler[%p]: job queue overflow detected", m_nc);
        }
        else {
          uint32_t dropcnt = m_jobqueue_overflow_dropcnt;
          ESP_LOGW(TAG, "WebSocketHandler[%p]: job queue overflow resolved, %" PRIu32 " drops", m_nc, dropcnt - m_jobqueue_overflow_dropcntref);
          m_jobqueue_overflow_dropcntref = dropcnt;
        }
      }
      break;
    
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
    case MG_EV_WRITE:
#else /* MG_VERSION_NUMBER */
    case MG_EV_SEND:
#endif /* MG_VERSION_NUMBER */
      // last transmission has finished
      ESP_EARLY_LOGV(TAG, "WebSocketHandler[%p] EV_SEND qlen=%d jobtype=%d sent=%d ack=%d", m_nc,
        m_jobqueue ? uxQueueMessagesWaiting(m_jobqueue) : -1, m_job.type, m_sent, m_ack);
      ContinueTx();
      break;
    
    default:
      break;
  }
  
  return ev;
}


void WebSocketHandler::HandleIncomingMsg(std::string msg)
{
  ESP_LOGD(TAG, "WebSocketHandler[%p]: received msg '%s'", m_nc, msg.c_str());
  
  std::istringstream input(msg);
  std::string cmd, arg;
  input >> cmd;
  
  if (cmd == "subscribe") {
    while (!input.eof()) {
      input >> arg;
      if (!arg.empty()) Subscribe(arg);
    }
  }
  else if (cmd == "unsubscribe") {
    while (!input.eof()) {
      input >> arg;
      if (!arg.empty()) Unsubscribe(arg);
    }
  }
  else {
    ESP_LOGW(TAG, "WebSocketHandler[%p]: unhandled message: '%s'", m_nc, msg.c_str());
  }
}


/**
 * OvmsWriter interface
 */

void WebSocketHandler::Log(LogBuffers* message)
{
  WebSocketTxJob job;
  job.type = WSTX_LogBuffers;
  job.logbuffers = message;
  if (!AddTxJob(job))
    message->release();
}


/**
 * WebSocketHandler slot registry:
 *  WebSocketSlots keep metrics modifiers once allocated (limited ressource)
 */

WebSocketHandler* OvmsWebServer::CreateWebSocketHandler(mg_connection* nc)
{
  if (xSemaphoreTake(m_client_mutex, portMAX_DELAY) != pdTRUE)
    return NULL;
  
  // find free slot:
  int i;
  for (i=0; i<m_client_slots.size() && m_client_slots[i].handler != NULL; i++);
  
  #undef bind  // Kludgy, but works
  using std::placeholders::_1;
  using std::placeholders::_2;
  
  if (i == m_client_slots.size()) {
    // create new client slot:
    WebSocketSlot slot;
    slot.handler = NULL;
    slot.modifier = MyMetrics.RegisterModifier();
    slot.reader = MyNotify.RegisterReader("ovmsweb", COMMAND_RESULT_VERBOSE,
                                          std::bind(&OvmsWebServer::IncomingNotification, i, _1, _2), true,
                                          std::bind(&OvmsWebServer::NotificationFilter, i, _1, _2));
    ESP_LOGD(TAG, "new WebSocket slot %d, registered modifier is %d, reader %d", i, slot.modifier, slot.reader);
    m_client_slots.push_back(slot);
  } else {
    // reuse slot:
    MyNotify.RegisterReader(m_client_slots[i].reader, "ovmsweb", COMMAND_RESULT_VERBOSE,
                            std::bind(&OvmsWebServer::IncomingNotification, i, _1, _2), true,
                            std::bind(&OvmsWebServer::NotificationFilter, i, _1, _2));
  }
  
  // create handler:
  WebSocketHandler* handler = new WebSocketHandler(nc, i, m_client_slots[i].modifier, m_client_slots[i].reader);
  m_client_slots[i].handler = handler;
  
  // start ticker:
  m_client_cnt++;
  if (m_client_cnt == 1)
    xTimerStart(m_update_ticker, 0);
  
  ESP_LOGD(TAG, "WebSocket[%p] handler %p opened; %d clients active", nc, handler, m_client_cnt);
  MyEvents.SignalEvent("server.web.socket.opened", (void*)m_client_cnt);
  
  xSemaphoreGive(m_client_mutex);
  
  // initial tx:
  handler->AddTxJob({ WSTX_MetricsAll, NULL });
  
  return handler;
}

void OvmsWebServer::DestroyWebSocketHandler(WebSocketHandler* handler)
{
  if (xSemaphoreTake(m_client_mutex, portMAX_DELAY) != pdTRUE)
    return;
  
  // find slot:
  for (int i=0; i<m_client_slots.size(); i++) {
    if (m_client_slots[i].handler == handler) {
      
      // stop ticker:
      m_client_cnt--;
      if (m_client_cnt == 0)
        xTimerStop(m_update_ticker, 0);
      
      // destroy handler:
      mg_connection* nc = handler->m_nc;
      m_client_slots[i].handler = NULL;
      delete handler;
      
      // clear unqueued notifications if any:
      MyNotify.ClearReader(m_client_slots[i].reader);
      
      ESP_LOGD(TAG, "WebSocket[%p] handler %p closed; %d clients active", nc, handler, m_client_cnt);
      MyEvents.SignalEvent("server.web.socket.closed", (void*)m_client_cnt);
      
      break;
    }
  }
  
  xSemaphoreGive(m_client_mutex);
}


bool OvmsWebServer::AddToBacklog(int client, WebSocketTxJob job)
{
  WebSocketTxTodo todo = { client, job };
  return (xQueueSend(MyWebServer.m_client_backlog, &todo, 0) == pdTRUE);
}


/**
 * EventListener:
 */
void OvmsWebServer::EventListener(std::string event, void* data)
{
  // shutdown delay to finish command output transmissions:
  if (event == "system.shuttingdown") {
    MyBoot.ShutdownPending("webserver");
    m_shutdown_countdown = 3;
  }

  // ticker:
  else if (event == "ticker.1") {
    #ifdef WEBSRV_HAVE_SETUPWIZARD
      CfgInitTicker();
    #endif
    if (m_shutdown_countdown > 0 && --m_shutdown_countdown == 0)
      MyBoot.ShutdownReady("webserver");
  }

  // reload plugins on changes:
  else if (event == "system.vfs.file.changed") {
    char* path = (char*)data;
    if (strncmp(path, "/store/plugin/", 14) == 0)
      ReloadPlugin(path);
  }

  // forward events to all websocket clients:
  if (xSemaphoreTake(m_client_mutex, 0) != pdTRUE) {
    // client list lock is not available, add to tx backlog:
    for (int i=0; i<m_client_cnt; i++) {
      auto& slot = m_client_slots[i];
      if (slot.handler) {
        WebSocketTxJob job = { WSTX_Event, strdup(event.c_str()) };
        if (!AddToBacklog(i, job)) {
          ESP_LOGW(TAG, "EventListener: event '%s' dropped for client %d", event.c_str(), i);
          free(job.event);
        }
      }
    }
    return;
  }
  
  // client list locked; add tx jobs:
  for (auto slot: m_client_slots) {
    if (slot.handler) {
      WebSocketTxJob job = { WSTX_Event, strdup(event.c_str()) };
      if (!slot.handler->AddTxJob(job, false))
        free(job.event);
      // Note: init_tx false to prevent mg_broadcast() deadlock on network events
      //  and keep processing time low
    }
  }
  
  xSemaphoreGive(m_client_mutex);
}


/**
 * UpdateTicker: periodical updates & tx queue checks
 * Note: this is executed in the timer task context. [https://www.freertos.org/RTOS-software-timer.html]
 */
void OvmsWebServer::UpdateTicker(TimerHandle_t timer)
{
  // Workaround for FreeRTOS duplicate timer callback bug
  // (see https://github.com/espressif/esp-idf/issues/8234)
  static TickType_t last_tick = 0;
  TickType_t tick = xTaskGetTickCount();
  if (tick < last_tick + xTimerGetPeriod(timer) - 3) return;
  last_tick = tick;

  if (xSemaphoreTake(MyWebServer.m_client_mutex, 0) != pdTRUE) {
    ESP_LOGD(TAG, "UpdateTicker: can't lock client list, ticker run skipped");
    return;
  }
  
  // check tx backlog:
  WebSocketTxTodo todo;
  while (xQueuePeek(MyWebServer.m_client_backlog, &todo, 0) == pdTRUE) {
    auto& slot = MyWebServer.m_client_slots[todo.client];
    if (!slot.handler) {
      // client is gone, discard job:
      todo.job.clear(todo.client);
      xQueueReceive(MyWebServer.m_client_backlog, &todo, 0);
    }
    else if (slot.handler->AddTxJob(todo.job)) {
      // job has been queued, remove:
      xQueueReceive(MyWebServer.m_client_backlog, &todo, 0);
    }
    else {
      // job queue is full: abort backlog processing
      break;
    }
  }

  // trigger metrics update if required.
  unsigned long mask_all = MyMetrics.GetUnitSendAll();
  for (auto slot: MyWebServer.m_client_slots) {
    if (slot.handler) {
      slot.handler->AddTxJob({ WSTX_MetricsUpdate, NULL });
      if (slot.handler->m_units_subscribed) {
        unsigned long bit = 1ul << slot.handler->m_modifier;
        bool addJob = (bit & mask_all) != 0;
        if (addJob) {
          // Trigger Units update:
          slot.handler->AddTxJob({ WSTX_UnitMetricUpdate, NULL });
        }
      }
      if (slot.handler->m_units_prefs_subscribed) {
        // Triger unit group config update.
        if (MyUnitConfig.HasModified(slot.handler->m_modifier))
          slot.handler->AddTxJob({ WSTX_UnitPrefsUpdate, NULL });
      }
    }
  }

  xSemaphoreGive(MyWebServer.m_client_mutex);
}

#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
// From https://github.com/cesanta/mongoose/blob/80d74e9e341d541f71c0fa587d22cec89be32dd5/src/mg_mqtt.c#
// Renamed and adapted to new Mongoose mg_str API
static struct mg_str ovms_mqtt_next_topic_component(struct mg_str *topic) {
  struct mg_str res = *topic;
  const char *c = mg_strstr(*topic, mg_str("/"));
  if (c != NULL) {
    res.len = (c - topic->ptr);
    topic->len -= (res.len + 1);
    topic->ptr += (res.len + 1);
  } else {
    topic->len = 0;
  }
  return res;
}

/* Refernce: https://mosquitto.org/man/mqtt-7.html */
int ovms_mqtt_match_topic_expression(struct mg_str exp, struct mg_str topic) {
  struct mg_str ec, tc;
  if (exp.len == 0) return 0;
  while (1) {
    ec = ovms_mqtt_next_topic_component(&exp);
    tc = ovms_mqtt_next_topic_component(&topic);
    if (ec.len == 0) {
      if (tc.len != 0) return 0;
      if (exp.len == 0) break;
      continue;
    }
    if (mg_vcmp(&ec, "+") == 0) {
      if (tc.len == 0 && topic.len == 0) return 0;
      continue;
    }
    if (mg_vcmp(&ec, "#") == 0) {
      /* Must be the last component in the expression or it's invalid. */
      return (exp.len == 0);
    }
    if (mg_strcmp(ec, tc) != 0) {
      return 0;
    }
  }
  return (tc.len == 0 && topic.len == 0);
}
#endif /* MG_VERSION_NUMBER */

/**
 * Notifications:
 */

void WebSocketHandler::Subscribe(std::string topic)
{
  for (auto it = m_subscriptions.begin(); it != m_subscriptions.end();) {
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
    if (ovms_mqtt_match_topic_expression(mg_str_n(topic.data(), topic.length()), mg_str_n((*it).data(), (*it).length()))) {
#else /* MG_VERSION_NUMBER */
    if (mg_mqtt_match_topic_expression(mg_mk_str(topic.c_str()), mg_mk_str((*it).c_str()))) {
#endif /* MG_VERSION_NUMBER */
      // remove topic covered by new subscription:
      ESP_LOGD(TAG, "WebSocketHandler[%p]: subscription '%s' removed", m_nc, (*it).c_str());
      it = m_subscriptions.erase(it);
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
    } else if (ovms_mqtt_match_topic_expression(mg_str_n((*it).data(), (*it).length()), mg_str_n(topic.data(), topic.length()))) {
#else /* MG_VERSION_NUMBER */
    } else if (mg_mqtt_match_topic_expression(mg_mk_str((*it).c_str()), mg_mk_str(topic.c_str()))) {
#endif /* MG_VERSION_NUMBER */
      // new subscription covered by existing:
      ESP_LOGD(TAG, "WebSocketHandler[%p]: subscription '%s' already covered by '%s'", m_nc, topic.c_str(), (*it).c_str());
      return;
    } else {
      it++;
    }
  }
  m_subscriptions.insert(topic);
  ESP_LOGD(TAG, "WebSocketHandler[%p]: subscription '%s' added", m_nc, topic.c_str());
  SubscriptionChanged();
}

void WebSocketHandler::Unsubscribe(std::string topic)
{
  bool changed = false;
  for (auto it = m_subscriptions.begin(); it != m_subscriptions.end();) {
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
    if (ovms_mqtt_match_topic_expression(mg_str_n(topic.data(), topic.length()), mg_str_n((*it).data(), (*it).length()))) {
#else /* MG_VERSION_NUMBER */
    if (mg_mqtt_match_topic_expression(mg_mk_str(topic.c_str()), mg_mk_str((*it).c_str()))) {
#endif /* MG_VERSION_NUMBER */
      ESP_LOGD(TAG, "WebSocketHandler[%p]: subscription '%s' removed", m_nc, (*it).c_str());
      it = m_subscriptions.erase(it);
      changed = true;
    } else {
      it++;
    }
  }
  if (changed)
    SubscriptionChanged();
}

void WebSocketHandler::SubscriptionChanged()
{
  UnitsCheckSubscribe();
  UnitsCheckVehicleSubscribe();
}

void WebSocketHandler::UnitsCheckSubscribe()
{
  bool newSubscribe = IsSubscribedTo("units/metrics");
  if (newSubscribe != m_units_subscribed) {
    m_units_subscribed = newSubscribe;
    if (newSubscribe) {
      ESP_LOGD(TAG, "WebSocketHandler[%p/%d]: Subscribed to units/metrics", m_nc, m_modifier);
      MyMetrics.SetAllUnitSend(m_modifier);
    } else {
      ESP_LOGD(TAG, "WebSocketHandler[%p/%d]: Unsubscribed from units/metrics", m_nc, m_modifier);
    }
  }
}

void WebSocketHandler::UnitsCheckVehicleSubscribe()
{
  bool newSubscribe = IsSubscribedTo("units/prefs");
  if (newSubscribe != m_units_prefs_subscribed) {
    m_units_prefs_subscribed = newSubscribe;
    if (newSubscribe) {
      ESP_LOGD(TAG, "WebSocketHandler[%p/%d]: Subscribed to units/prefs", m_nc, m_modifier);
      MyUnitConfig.InitialiseSlot(m_modifier);
    } else {
      ESP_LOGD(TAG, "WebSocketHandler[%p/%d]: Unsubscribed from units/prefs", m_nc, m_modifier);
    }
  }
}

bool WebSocketHandler::IsSubscribedTo(std::string topic)
{
  for (auto it = m_subscriptions.begin(); it != m_subscriptions.end(); it++) {
#if MG_VERSION_NUMBER >= MG_VERSION_VAL(7, 0, 0)
    if (ovms_mqtt_match_topic_expression(mg_str_n((*it).data(), (*it).length()), mg_str_n(topic.data(), topic.length()))) {
#else /* MG_VERSION_NUMBER */
    if (mg_mqtt_match_topic_expression(mg_mk_str((*it).c_str()), mg_mk_str(topic.c_str()))) {
#endif /* MG_VERSION_NUMBER */
      return true;
    }
  }
  return false;
}

bool OvmsWebServer::NotificationFilter(int client, OvmsNotifyType* type, const char* subtype)
{
  if (xSemaphoreTake(MyWebServer.m_client_mutex, 0) != pdTRUE) {
    return true; // assume subscription (safe side)
  }

  bool accept = false;
  const auto& slot = MyWebServer.m_client_slots[client];

  if (slot.handler == NULL)
  {
    // client gone:
    accept = false;
  }
  else if (strcmp(type->m_name, "info") == 0 ||
           strcmp(type->m_name, "error") == 0 ||
           strcmp(type->m_name, "alert") == 0)
  {
    // always forward these:
    accept = true;
  }
  else if (strcmp(type->m_name, "data") == 0 ||
           strcmp(type->m_name, "stream") == 0)
  {
    // forward if subscribed:
    std::string topic = std::string("notify/") + type->m_name + "/" + mqtt_topic(subtype);
    accept = slot.handler->IsSubscribedTo(topic);
  }

  xSemaphoreGive(MyWebServer.m_client_mutex);
  return accept;
}

bool OvmsWebServer::IncomingNotification(int client, OvmsNotifyType* type, OvmsNotifyEntry* entry)
{
  WebSocketTxJob job;
  job.type = WSTX_Notify;
  job.notification = entry;
  bool done = false;

  if (xSemaphoreTake(MyWebServer.m_client_mutex, 0) != pdTRUE) {
    if (!MyWebServer.AddToBacklog(client, job)) {
      ESP_LOGW(TAG, "IncomingNotification of type '%s' subtype '%s' dropped for client %d",
               type->m_name, entry->GetSubType(), client);
      done = true;
    }
    return done;
  }

  const auto& slot = MyWebServer.m_client_slots[client];
  if (!slot.handler || !slot.handler->AddTxJob(job, false))
    done = true;

  xSemaphoreGive(MyWebServer.m_client_mutex);
  return done;
}
