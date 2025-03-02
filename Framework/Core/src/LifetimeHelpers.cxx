// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

#include "Framework/DataProcessingHeader.h"
#include "Framework/InputSpec.h"
#include "Framework/LifetimeHelpers.h"
#include "Framework/DataDescriptorMatcher.h"
#include "Framework/Logger.h"
#include "Framework/RawDeviceService.h"
#include "Framework/ServiceRegistry.h"
#include "Framework/TimesliceIndex.h"
#include "Framework/VariableContextHelpers.h"
#include "Framework/DataTakingContext.h"

#include "Headers/DataHeader.h"
#include "Headers/DataHeaderHelpers.h"
#include "Headers/Stack.h"
#include "CommonConstants/LHCConstants.h"
#include "MemoryResources/MemoryResources.h"
#include "CCDB/CcdbApi.h"
#include <typeinfo>
#include <TError.h>
#include <TMemFile.h>
#include <curl/curl.h>

#include <fairmq/FairMQDevice.h>

#include <cstdlib>

using namespace o2::header;
using namespace fair;

namespace o2::framework
{

namespace
{
size_t getCurrentTime()
{
  auto now = std::chrono::system_clock::now();
  auto duration = now.time_since_epoch();
  return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}
} // namespace

ExpirationHandler::Creator LifetimeHelpers::dataDrivenCreation()
{
  return [](TimesliceIndex& index) -> TimesliceSlot {
    return {TimesliceSlot::ANY};
  };
}

ExpirationHandler::Creator LifetimeHelpers::enumDrivenCreation(size_t start, size_t end, size_t step, size_t inputTimeslice, size_t maxInputTimeslices)
{
  auto last = std::make_shared<size_t>(start + inputTimeslice * step);
  return [start, end, step, last, inputTimeslice, maxInputTimeslices](TimesliceIndex& index) -> TimesliceSlot {
    for (size_t si = 0; si < index.size(); si++) {
      if (*last > end) {
        return TimesliceSlot{TimesliceSlot::INVALID};
      }
      auto slot = TimesliceSlot{si};
      if (index.isValid(slot) == false) {
        TimesliceId timestamp{*last};
        *last += step * maxInputTimeslices;
        index.associate(timestamp, slot);
        return slot;
      }
    }
    return TimesliceSlot{TimesliceSlot::INVALID};
  };
}

ExpirationHandler::Creator LifetimeHelpers::timeDrivenCreation(std::chrono::microseconds period)
{
  auto start = getCurrentTime();
  auto last = std::make_shared<decltype(start)>(start);
  // FIXME: should create timeslices when period expires....
  return [last, period](TimesliceIndex& index) -> TimesliceSlot {
    // Nothing to do if the time has not expired yet.
    auto current = getCurrentTime();
    auto delta = current - *last;
    if (delta < period.count()) {
      return TimesliceSlot{TimesliceSlot::INVALID};
    }
    // We first check if the current time is not already present
    // FIXME: this should really be done by query matching? Ok
    //        for now to avoid duplicate entries.
    for (size_t i = 0; i < index.size(); ++i) {
      TimesliceSlot slot{i};
      if (index.isValid(slot) == false) {
        continue;
      }
      auto& variables = index.getVariablesForSlot(slot);
      if (VariableContextHelpers::getTimeslice(variables).value == current) {
        return TimesliceSlot{TimesliceSlot::INVALID};
      }
    }
    // If we are here the timer has expired and a new slice needs
    // to be created.
    *last = current;
    data_matcher::VariableContext newContext;
    newContext.put({0, static_cast<uint64_t>(current)});
    newContext.commit();
    auto [action, slot] = index.replaceLRUWith(newContext, TimesliceId{current});
    switch (action) {
      case TimesliceIndex::ActionTaken::ReplaceObsolete:
      case TimesliceIndex::ActionTaken::ReplaceUnused:
        index.associate(TimesliceId{current}, slot);
        break;
      case TimesliceIndex::ActionTaken::DropInvalid:
      case TimesliceIndex::ActionTaken::DropObsolete:
      case TimesliceIndex::ActionTaken::Wait:
        break;
    }
    return slot;
  };
}

ExpirationHandler::Checker LifetimeHelpers::expireNever()
{
  return [](ServiceRegistry&, int64_t) -> bool { return false; };
}

ExpirationHandler::Checker LifetimeHelpers::expireAlways()
{
  return [](ServiceRegistry&, int64_t) -> bool { return true; };
}

ExpirationHandler::Checker LifetimeHelpers::expireTimed(std::chrono::microseconds period)
{
  auto start = getCurrentTime();
  auto last = std::make_shared<decltype(start)>(start);
  return [last, period](ServiceRegistry&, int64_t) -> bool {
    auto current = getCurrentTime();
    auto delta = current - *last;
    if (delta > period.count()) {
      *last = current;
      return true;
    }
    return false;
  };
}

/// Does nothing. Use this for cases where you do not want to do anything
/// when records expire. This is the default behavior for data (which never
/// expires via this mechanism).
ExpirationHandler::Handler LifetimeHelpers::doNothing()
{
  return [](ServiceRegistry&, PartRef& ref, data_matcher::VariableContext&) -> void { return; };
}

// We simply put everything
size_t readToBuffer(void* p, size_t size, size_t nmemb, void* userdata)
{
  if (nmemb == 0) {
    return 0;
  }
  if (size == 0) {
    return 0;
  }
  std::vector<char>* buffer = (std::vector<char>*)userdata;
  size_t oldSize = buffer->size();
  buffer->resize(oldSize + nmemb * size);
  memcpy(buffer->data() + oldSize, p, nmemb * size);
  return size * nmemb;
}

// We simply put everything in a stringstream and read it afterwards.
size_t readToMessage(void* p, size_t size, size_t nmemb, void* userdata)
{
  if (nmemb == 0) {
    return 0;
  }
  if (size == 0) {
    return 0;
  }
  o2::vector<char>* buffer = (o2::vector<char>*)userdata;
  size_t oldSize = buffer->size();
  buffer->resize(oldSize + nmemb * size);
  memcpy(buffer->data() + oldSize, p, nmemb * size);
  return size * nmemb;
}

ExpirationHandler::Checker
  LifetimeHelpers::expectCTP(std::string const& serverUrl, bool waitForCTP)
{
  return [serverUrl, waitForCTP](ServiceRegistry& services, int64_t timestamp) -> bool {
    auto& dataTakingContext = services.get<DataTakingContext>();
    if (waitForCTP == false || dataTakingContext.source == OrbitResetTimeSource::CTP) {
      return true;
    }
    LOG(INFO) << "CTP is not there, fetching.";
    std::vector<char> buffer;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
      throw runtime_error("fetchFromCCDBCache: Unable to initialise CURL");
    }
    CURLcode res;
    std::string path = "CTP/OrbitReset";
    auto url = fmt::format("{}/{}/{}", serverUrl, path, timestamp / 1000);
    LOG(INFO) << "Fetching CTP from " << url;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, readToBuffer);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, true);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      throw runtime_error_f("Unable to fetch %s from CCDB", url.c_str());
    }
    long responseCode;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);

    if (responseCode != 200) {
      throw runtime_error_f("HTTP error %d while fetching %s from CCDB", responseCode, url.c_str());
    }

    curl_easy_cleanup(curl);

    Int_t previousErrorLevel = gErrorIgnoreLevel;
    gErrorIgnoreLevel = kFatal;
    TMemFile memFile("name", const_cast<char*>(buffer.data()), buffer.size(), "READ");
    gErrorIgnoreLevel = previousErrorLevel;
    if (memFile.IsZombie()) {
      return false;
    }
    TClass* tcl = TClass::GetClass(typeid(std::vector<Long64_t>));
    void* result = ccdb::CcdbApi::extractFromTFile(memFile, tcl);
    if (!result) {
      throw runtime_error_f("Couldn't retrieve object corresponding to %s from TFile", tcl->GetName());
    }
    memFile.Close();
    std::vector<Long64_t>* ctp = (std::vector<Long64_t>*)result;
    LOG(INFO) << "Orbit reset time now at " << (*ctp)[0];
    dataTakingContext.orbitResetTime = (*ctp)[0];
    dataTakingContext.source = OrbitResetTimeSource::CTP;
    return true;
  };
}

/// Fetch an object from CCDB if the record is expired. The actual
/// name of the object is given by:
///
/// "<namespace>/<InputRoute.origin>/<InputRoute.description>"
///
/// \todo for the moment we always go to CCDB every time we are expired.
/// \todo this should really be done in the common fetcher.
/// \todo provide a way to customize the namespace from the ProcessingContext
ExpirationHandler::Handler
  LifetimeHelpers::fetchFromCCDBCache(InputSpec const& spec,
                                      std::string const& prefix,
                                      std::string const& overrideTimestamp,
                                      std::string const& sourceChannel)
{
  char* err;
  uint64_t overrideTimestampMilliseconds = strtoll(overrideTimestamp.c_str(), &err, 10);
  if (*err != 0) {
    throw runtime_error("fetchFromCCDBCache: Unable to parse forced timestamp for conditions");
  }
  if (overrideTimestampMilliseconds) {
    LOGP(info, "fetchFromCCDBCache: forcing timestamp for conditions to {} milliseconds from epoch UTC", overrideTimestampMilliseconds);
  }
  auto matcher = std::get_if<ConcreteDataMatcher>(&spec.matcher);
  if (matcher == nullptr) {
    throw runtime_error("InputSpec for Conditions must be fully qualified");
  }
  return [spec, matcher, sourceChannel, serverUrl = prefix, overrideTimestampMilliseconds](ServiceRegistry& services, PartRef& ref, data_matcher::VariableContext& variables) -> void {
    // We should invoke the handler only once.
    assert(!ref.header);
    assert(!ref.payload);

    auto& rawDeviceService = services.get<RawDeviceService>();
    auto& dataTakingContext = services.get<DataTakingContext>();

    auto&& transport = rawDeviceService.device()->GetChannel(sourceChannel, 0).Transport();
    auto channelAlloc = o2::pmr::getTransportAllocator(transport);
    o2::vector<char> payloadBuffer{transport->GetMemoryResource()};
    payloadBuffer.reserve(10000); // we begin with messages of 10KB

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
      throw runtime_error("fetchFromCCDBCache: Unable to initialise CURL");
    }
    CURLcode res;

    // * By default we use the time when the data was created.
    // * If an override is specified, we use it.
    // * If the orbit reset time comes from CTP, we use it for precise
    //   timestamp evaluation via the firstTFOrbit
    uint64_t timestamp = -1;
    if (overrideTimestampMilliseconds) {
      timestamp = overrideTimestampMilliseconds;
    } else if (dataTakingContext.source == OrbitResetTimeSource::CTP) {
      // Orbit reset time is in microseconds, LHCOrbitNS is in nanoseconds, CCDB uses milliseconds
      timestamp = ceilf((VariableContextHelpers::getFirstTFOrbit(variables) * o2::constants::lhc::LHCOrbitNS / 1000 + dataTakingContext.orbitResetTime) / 1000);
    } else {
      // The timestamp used by DPL is in nanoseconds
      timestamp = ceilf(VariableContextHelpers::getTimeslice(variables).value / 1000);
    }

    std::string path = "";
    bool runDependent = false;
    for (auto& meta : spec.metadata) {
      if (meta.name == "ccdb-path") {
        path = meta.defaultValue.get<std::string>();
      }
      if (meta.name == "ccdb-run-dependent") {
        runDependent = meta.defaultValue.get<bool>();
      }
    }
    if (path.empty()) {
      path = fmt::format("{}/{}", matcher->origin, matcher->description);
    }
    std::string url;
    if (runDependent == false) {
      url = fmt::format("{}/{}/{}", serverUrl, path, timestamp);
    } else {
      url = fmt::format("{}/{}/{}/runNumber={}", serverUrl, path, timestamp, dataTakingContext.runNumber);
    }
    LOG(INFO) << "fetchFromCCDBCache: Fetching " << url;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &payloadBuffer);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, readToMessage);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, true);

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      throw runtime_error_f("fetchFromCCDBCache: Unable to fetch %s from CCDB", url.c_str());
    }
    long responseCode;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);

    if (responseCode != 200) {
      throw runtime_error_f("fetchFromCCDBCache: HTTP error %d while fetching %s from CCDB", responseCode, url.c_str());
    }

    curl_easy_cleanup(curl);

    DataHeader dh;
    dh.dataOrigin = matcher->origin;
    dh.dataDescription = matcher->description;
    dh.subSpecification = matcher->subSpec;
    // FIXME: should use curl_off_t and CURLINFO_SIZE_DOWNLOAD_T, but
    //        apparently not there on some platforms.
    double dl;
    res = curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD, &dl);
    dh.payloadSize = payloadBuffer.size();
    dh.payloadSerializationMethod = gSerializationMethodCCDB;

    DataProcessingHeader dph{timestamp, 1};
    auto header = o2::pmr::getMessage(o2::header::Stack{channelAlloc, dh, dph});
    auto payload = o2::pmr::getMessage(std::forward<o2::vector<char>>(payloadBuffer), transport->GetMemoryResource());

    ref.header = std::move(header);
    ref.payload = std::move(payload);

    return;
  };
}

/// Create an entry in the registry for histograms on the first
/// FIXME: actually implement this
/// FIXME: provide a way to customise the histogram from the configuration.
ExpirationHandler::Handler LifetimeHelpers::fetchFromQARegistry()
{
  return [](ServiceRegistry&, PartRef& ref, data_matcher::VariableContext&) -> void {
    throw runtime_error("fetchFromQARegistry: Not yet implemented");
    return;
  };
}

/// Create an entry in the registry for histograms on the first
/// FIXME: actually implement this
/// FIXME: provide a way to customise the histogram from the configuration.
ExpirationHandler::Handler LifetimeHelpers::fetchFromObjectRegistry()
{
  return [](ServiceRegistry&, PartRef& ref, data_matcher::VariableContext&) -> void {
    throw runtime_error("fetchFromObjectRegistry: Not yet implemented");
    return;
  };
}

/// Enumerate entries on every invokation.
ExpirationHandler::Handler LifetimeHelpers::enumerate(ConcreteDataMatcher const& matcher, std::string const& sourceChannel,
                                                      int64_t orbitOffset, int64_t orbitMultiplier)
{
  using counter_t = int64_t;
  auto counter = std::make_shared<counter_t>(0);
  return [matcher, counter, sourceChannel, orbitOffset, orbitMultiplier](ServiceRegistry& services, PartRef& ref, data_matcher::VariableContext& variables) -> void {
    // We should invoke the handler only once.
    assert(!ref.header);
    assert(!ref.payload);
    auto& rawDeviceService = services.get<RawDeviceService>();

    auto timestamp = VariableContextHelpers::getTimeslice(variables).value;
    DataHeader dh;
    dh.dataOrigin = matcher.origin;
    dh.dataDescription = matcher.description;
    dh.subSpecification = matcher.subSpec;
    dh.payloadSize = sizeof(counter_t);
    dh.payloadSerializationMethod = gSerializationMethodNone;
    dh.tfCounter = timestamp;
    dh.firstTForbit = timestamp * orbitMultiplier + orbitOffset;
    variables.put({data_matcher::FIRSTTFORBIT_POS, dh.firstTForbit});
    variables.put({data_matcher::TFCOUNTER_POS, dh.tfCounter});

    DataProcessingHeader dph{timestamp, 1};

    auto&& transport = rawDeviceService.device()->GetChannel(sourceChannel, 0).Transport();
    auto channelAlloc = o2::pmr::getTransportAllocator(transport);
    auto header = o2::pmr::getMessage(o2::header::Stack{channelAlloc, dh, dph});
    ref.header = std::move(header);

    auto payload = rawDeviceService.device()->NewMessage(sizeof(counter_t));
    *(counter_t*)payload->GetData() = *counter;
    ref.payload = std::move(payload);
    (*counter)++;
  };
}

/// Create a dummy message with the provided ConcreteDataMatcher
ExpirationHandler::Handler LifetimeHelpers::dummy(ConcreteDataMatcher const& matcher, std::string const& sourceChannel)
{
  using counter_t = int64_t;
  auto counter = std::make_shared<counter_t>(0);
  auto f = [matcher, counter, sourceChannel](ServiceRegistry& services, PartRef& ref, data_matcher::VariableContext& variables) -> void {
    // We should invoke the handler only once.
    assert(!ref.header);
    assert(!ref.payload);
    auto& rawDeviceService = services.get<RawDeviceService>();

    auto timestamp = VariableContextHelpers::getTimeslice(variables).value;
    DataHeader dh;
    dh.dataOrigin = matcher.origin;
    dh.dataDescription = matcher.description;
    dh.subSpecification = matcher.subSpec;
    dh.payloadSize = 0;
    dh.payloadSerializationMethod = gSerializationMethodNone;

    {
      auto pval = std::get_if<uint32_t>(&variables.get(data_matcher::FIRSTTFORBIT_POS));
      if (pval == nullptr) {
        dh.firstTForbit = -1;
      } else {
        dh.firstTForbit = *pval;
      }
    }
    {
      auto pval = std::get_if<uint32_t>(&variables.get(data_matcher::TFCOUNTER_POS));
      if (pval == nullptr) {
        dh.tfCounter = timestamp;
      } else {
        dh.tfCounter = *pval;
      }
    }

    DataProcessingHeader dph{timestamp, 1};

    auto&& transport = rawDeviceService.device()->GetChannel(sourceChannel, 0).Transport();
    auto channelAlloc = o2::pmr::getTransportAllocator(transport);
    auto header = o2::pmr::getMessage(o2::header::Stack{channelAlloc, dh, dph});
    ref.header = std::move(header);
    auto payload = rawDeviceService.device()->NewMessage(0);
    ref.payload = std::move(payload);
  };
  return f;
}

} // namespace o2::framework
