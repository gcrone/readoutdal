/**
 * @file generate_modules.cpp
 *
 * Implementation of ReadoutApplication's generate_modules dal method
 *
 * This is part of the DUNE DAQ Software Suite, copyright 2023.
 * Licensing/copyright details are in the COPYING file that you should have
 * received with this code.
 */

#include "ModuleFactory.hpp"

#include "oksdbinterfaces/Configuration.hpp"
#include "oks/kernel.hpp"

#include "coredal/Connection.hpp"
#include "coredal/NetworkConnection.hpp"
#include "coredal/ResourceSet.hpp"
#include "coredal/Session.hpp"

#include "readoutdal/DataReader.hpp"
#include "readoutdal/DataReaderConf.hpp"
#include "readoutdal/DLH.hpp"
#include "readoutdal/DROStreamConf.hpp"
#include "readoutdal/LinkHandlerConf.hpp"
#include "readoutdal/NetworkConnectionRule.hpp"
#include "readoutdal/NetworkConnectionDescriptor.hpp"
#include "readoutdal/QueueConnectionRule.hpp"
#include "readoutdal/QueueDescriptor.hpp"
#include "readoutdal/ReadoutApplication.hpp"
#include "readoutdal/ReadoutGroup.hpp"
#include "readoutdal/TPHandler.hpp"
#include "readoutdal/TPHandlerConf.hpp"

#include "readoutdalIssues.hpp"

#include "logging/Logging.hpp"

#include <string>
#include <vector>

using namespace dunedaq;
using namespace dunedaq::readoutdal;

static ModuleFactory::Registrator
__reg__("ReadoutApplication", [] (const SmartDaqApplication* smartApp,
                                  oksdbinterfaces::Configuration* confdb,
                                  const std::string& dbfile,
                                  const coredal::Session* session) -> ModuleFactory::ReturnType
  {
    auto app = smartApp->cast<ReadoutApplication>();
    return app->generate_modules(confdb, dbfile, session);
  });

std::vector<const coredal::DaqModule*> 
ReadoutApplication::generate_modules(oksdbinterfaces::Configuration* confdb,
                                     const std::string& dbfile,
                                     const coredal::Session* session) const {
  //oks::OksFile::set_nolock_mode(true);

  std::vector<const coredal::DaqModule*> modules;

  auto dlhConf = get_link_handler();
  auto dlhClass = dlhConf->get_template_for();

  // Process the queue rules looking for inputs to our DL/TP handler modules
  const QueueDescriptor* dlhInputQDesc = nullptr;
  const QueueDescriptor* tpInputQDesc = nullptr;
  for (auto rule : get_queue_rules()) {
    auto destination_class = rule->get_destination_class();
    if (destination_class == "DLH" || destination_class == dlhClass) {
      dlhInputQDesc = rule->get_descriptor();
    }
    else if (destination_class == "TPHandler") {
      tpInputQDesc = rule->get_descriptor();
    }
  }

  // Process the network rules looking for the DL/TP handler data reuest inputs
  const NetworkConnectionDescriptor* dlhNetDesc = nullptr;
  const NetworkConnectionDescriptor* tpNetDesc = nullptr;
  for (auto rule : get_network_rules()) {
    auto endpoint_class = rule->get_endpoint_class();
    if (endpoint_class == "DLH" || endpoint_class == dlhClass) {
      dlhNetDesc = rule->get_descriptor();
    }
    else if (endpoint_class == "TPHandler") {
      tpNetDesc = rule->get_descriptor();
    }
  }

  // Now create the TP Handler and its associated queue and network
  // connections if we have a TP handler config
  oksdbinterfaces::ConfigObject tpQueueObj;
  oksdbinterfaces::ConfigObject tpNetObj;
  auto tpHandlerConf = get_tp_handler();
  if (tpHandlerConf) {
    if (tpNetDesc == nullptr) {
      throw (BadConf(ERS_HERE, "No tpHandler network descriptor given"));
    }
    if (tpInputQDesc == nullptr) {
      throw (BadConf(ERS_HERE, "No tpHandler input queue descriptor given"));
    }
    auto tpsrc = get_tp_src_id();
    if (tpsrc == 0) {
      throw (BadConf(ERS_HERE, "No TPHandler src_id given"));
    }
    std::string tpQueueUid("inputToTPH-"+std::to_string(tpsrc));
    confdb->create(dbfile, "Queue", tpQueueUid, tpQueueObj);
    tpQueueObj.set_by_val<std::string>("data_type", tpInputQDesc->get_data_type());
    tpQueueObj.set_by_val<std::string>("queue_type", tpInputQDesc->get_queue_type());
    tpQueueObj.set_by_val<uint32_t>("capacity", tpInputQDesc->get_capacity());

    std::string tpNetUid("ReqToTPH-"+std::to_string(tpsrc));
    confdb->create(dbfile, "NetworkConnection", tpNetUid, tpNetObj);
    tpNetObj.set_by_val<std::string>("data_type", tpNetDesc->get_data_type());
    tpNetObj.set_by_val<std::string>("connection_type", tpNetDesc->get_connection_type());
    tpNetObj.set_by_val<std::string>("uri", tpNetDesc->get_uri());
    tpNetObj.set_by_val<uint16_t>("port", tpNetDesc->get_port());
    
    auto tphConfObj = tpHandlerConf->config_object();
    oksdbinterfaces::ConfigObject tpObj;
    std::string tpUid("tphandler-"+std::to_string(tpsrc));
    confdb->create(dbfile, "TPHandler", tpUid, tpObj);
    tpObj.set_by_val<uint32_t>("source_id", tpsrc);
    tpObj.set_obj("handler_configuration", &tphConfObj);
    tpObj.set_objs("inputs", {&tpQueueObj, &tpNetObj});

    // Add to our list of modules to return
    modules.push_back(confdb->get<TPHandler>(tpUid));
  }

  // Now create the DataReader objects, one per group of data streams
  auto rdrConf = get_data_reader();
  if (rdrConf == 0) {
    throw (BadConf(ERS_HERE, "No DataReader configuration given"));
  }

  int rnum = 0;
  int port_offset = 0;
  // Create a DataReader for each (non-disabled) group and a Data Link
  // Handler for each stream of this DataReader
  //for (auto roGroup : get_readout_groups()) {
  for (auto roGroup : get_contains()) {
    if (roGroup->disabled(*session)) {
      TLOG_DEBUG(7) << "Ignoring disabled ReadoutGroup " << roGroup->UID();
      continue;
    }
    auto rset = roGroup->cast<ReadoutGroup>();
    if (rset == nullptr) {
        throw (BadConf(ERS_HERE, "ReadoutApplication contains something other than ReadoutGroup"));
    }
    std::vector<const coredal::Connection*> outputQueues;
    for (auto res : rset->get_contains()) {
      auto stream = res->cast<DROStreamConf>();
      if (stream == nullptr) {
        throw (BadConf(ERS_HERE, "ReadoutGroup contains something other than DROStreamConf"));
      }
      if (stream->disabled(*session)) {
        TLOG_DEBUG(7) << "Ignoring disabled DROStreamConf " << stream->UID();
        continue;
      }
      auto id = stream->get_src_id();
      std::string uid("DLH-"+std::to_string(id));
      oksdbinterfaces::ConfigObject dlhObj;
      TLOG_DEBUG(7) <<  "creating OKS configuration object for Data Link Handler class " << dlhClass;
      confdb->create(dbfile, dlhClass, uid, dlhObj);
      dlhObj.set_by_val<uint32_t>("source_id", id);
      dlhObj.set_obj("handler_configuration", &dlhConf->config_object());
      if (tpHandlerConf) {
        dlhObj.set_objs("outputs", {&tpQueueObj});
      }
      std::string queueUid("inputToDLH-"+std::to_string(id));
      oksdbinterfaces::ConfigObject queueObj;
      confdb->create(dbfile, "Queue", queueUid, queueObj);
      queueObj.set_by_val<std::string>("data_type", dlhInputQDesc->get_data_type());
      queueObj.set_by_val<std::string>("queue_type", dlhInputQDesc->get_queue_type());
      queueObj.set_by_val<uint32_t>("capacity", dlhInputQDesc->get_capacity());

      std::ostringstream uidStream;
      uidStream.fill('0');
      uidStream << dlhNetDesc->get_uid_base() << std::hex << std::setw(8) << id;
      std::string netUid=uidStream.str();
      oksdbinterfaces::ConfigObject netObj;
      confdb->create(dbfile, "NetworkConnection", netUid, netObj);
      netObj.set_by_val<std::string>("data_type", dlhNetDesc->get_data_type());
      netObj.set_by_val<std::string>("connection_type", dlhNetDesc->get_connection_type());
      netObj.set_by_val<std::string>("uri", dlhNetDesc->get_uri());
      uint16_t port = dlhNetDesc->get_port();
      port = port ? port+port_offset : port;
      netObj.set_by_val<uint16_t>("port", port);

      port_offset++;
      std::vector<const oksdbinterfaces::ConfigObject*> inputObjs{
        &(confdb->get<coredal::Connection>(queueUid)->config_object()),
        &(confdb->get<coredal::Connection>(netUid)->config_object())
      };
      dlhObj.set_objs("inputs", inputObjs);

      // Add the input queue dal pointer to the outputs of the DataReader
      outputQueues.push_back(confdb->get<coredal::Connection>(queueUid));

      modules.push_back(confdb->get<DLH>(uid));
    }

    std::string readerUid("datareader-"+UID()+"-"+std::to_string(rnum++));
    std::string readerClass = rdrConf->get_template_for();
    oksdbinterfaces::ConfigObject readerObj;
    TLOG_DEBUG(7) <<  "creating OKS configuration object for Data reader class " << readerClass;
    confdb->create(dbfile, readerClass, readerUid, readerObj);

    std::vector<const oksdbinterfaces::ConfigObject*> qObjs;
    for (auto q : outputQueues) {
      qObjs.push_back(&q->config_object());
    }
    readerObj.set_objs("outputs", qObjs);
    readerObj.set_obj("configuration", &rdrConf->config_object());

    modules.push_back(confdb->get<DataReader>(readerUid));
  }
  //oks::OksFile::set_nolock_mode(false);
  return modules;
}
