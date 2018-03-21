// Copyright (c) 2015, Lawrence Livermore National Security, LLC.
// Produced at the Lawrence Livermore National Laboratory.
//
// This file is part of Caliper.
// Written by David Boehme, boehme3@llnl.gov.
// LLNL-CODE-678900
// All rights reserved.
//
// For details, see https://github.com/scalability-llnl/Caliper.
// Please also see the LICENSE file for our additional BSD notice.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
//  * Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the disclaimer below.
//  * Redistributions in binary form must reproduce the above copyright notice, this list of
//    conditions and the disclaimer (as noted below) in the documentation and/or other materials
//    provided with the distribution.
//  * Neither the name of the LLNS/LLNL nor the names of its contributors may be used to endorse
//    or promote products derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS
// OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// LAWRENCE LIVERMORE NATIONAL SECURITY, LLC, THE U.S. DEPARTMENT OF ENERGY OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// Cupti.cpp
// Implementation of Cupti service

#include "CuptiEventSampling.h"

#include "caliper/CaliperService.h"

#include "caliper/Caliper.h"
#include "caliper/SnapshotRecord.h"

#include "caliper/common/Log.h"
#include "caliper/common/RuntimeConfig.h"

#include <cupti.h>
#include <nvToolsExt.h>
#include <generated_nvtx_meta.h>

#include <vector>

using namespace cali;

namespace
{
    //
    // --- Data
    //

    ConfigSet              config;
    const ConfigSet::Entry configdata[] = {
        { "callback_domains", CALI_TYPE_STRING, "runtime:sync",
          "List of CUDA callback domains to capture",
          "List of CUDA callback domains to capture. Possible values:\n"
          "  runtime  :  Capture CUDA runtime API calls\n"
          "  driver   :  Capture CUDA driver calls\n"
          "  resource :  Capture CUDA resource creation events\n"
          "  sync     :  Capture CUDA synchronization events\n"
          "  nvtx     :  Capture NVidia NVTX annotations\n"
          "  none     :  Don't capture callbacks"
        },
        { "record_symbol", CALI_TYPE_BOOL, "true",
          "Record symbol name (kernel) for CUDA runtime and driver callbacks",
          "Record symbol name (kernel) for CUDA runtime and driver callbacks"
        },
        { "record_context", CALI_TYPE_BOOL, "true",
          "Record CUDA context ID for CUDA runtime and driver callbacks",
          "Record CUDA context ID for CUDA runtime and driver callbacks"
        },
        { "sample_events", CALI_TYPE_STRING, "",
          "CUpti events to sample",
          "CUpti events to sample"
        },
        { "sample_event_id", CALI_TYPE_UINT, "0",
          "CUpti event ID to sample",
          "CUpti event ID to sample"
        },

        ConfigSet::Terminator
    };

    const struct CallbackDomainInfo {
        CUpti_CallbackDomain domain;
        const char* name;
    } callback_domains[] = {
        { CUPTI_CB_DOMAIN_RUNTIME_API, "runtime"  },
        { CUPTI_CB_DOMAIN_DRIVER_API,  "driver"   },
        { CUPTI_CB_DOMAIN_RESOURCE,    "resource" },
        { CUPTI_CB_DOMAIN_SYNCHRONIZE, "sync"     },
        { CUPTI_CB_DOMAIN_NVTX,        "nvtx"     },
        { CUPTI_CB_DOMAIN_INVALID,     "none"     },
        { CUPTI_CB_DOMAIN_INVALID,     0          }
    };

    struct CuptiServiceInfo {
        Attribute runtime_attr;
        Attribute driver_attr;
        Attribute resource_attr;
        Attribute sync_attr;
        Attribute nvtx_range_attr;

        Attribute context_attr;
        Attribute symbol_attr;
        Attribute device_attr;
        Attribute stream_attr;

        bool      record_context;
        bool      record_symbol;
    }                      cupti_info;

    CUpti_SubscriberHandle subscriber;

    unsigned               num_cb;
    unsigned               num_api_cb;
    unsigned               num_resource_cb;
    unsigned               num_sync_cb;
    unsigned               num_nvtx_cb;

    Cupti::EventSampling   event_sampling;
    
    //
    // --- Helper functions
    //

    void
    print_cupti_error(std::ostream& os, CUptiResult err, const char* func)
    {
        const char* errstr;

        cuptiGetResultString(err, &errstr);

        os << "cupti: " << func << ": error: " << errstr << std::endl;
    }

    //
    // --- CUPTI Callback handling
    //

    void
    handle_stream_event(CUcontext context, CUstream stream, const Attribute& name_attr, const Variant& v_name)
    {
        uint32_t  context_id = 0;
        uint32_t  stream_id  = 0;
        uint32_t  device_id  = 0;

        if (cuptiGetDeviceId(context,  &device_id)  != CUPTI_SUCCESS)
            return;
        if (cuptiGetContextId(context, &context_id) != CUPTI_SUCCESS)
            return;
        // TODO: Use cuptiGetStreamIdEx() for CUDA 8.0+
        if (cuptiGetStreamId(context, stream, &stream_id) != CUPTI_SUCCESS)
            return;

        Attribute attr[4] = { cupti_info.device_attr,
                              cupti_info.context_attr,
                              cupti_info.stream_attr,
                              name_attr   };
        Variant   vals[4] = { Variant(static_cast<uint64_t>(device_id)),
                              Variant(static_cast<uint64_t>(context_id)),
                              Variant(static_cast<uint64_t>(stream_id)),
                              v_name };

        SnapshotRecord::FixedSnapshotRecord<4> trigger_info_data;
        SnapshotRecord trigger_info(trigger_info_data);

        Caliper c;

        c.make_entrylist(4, attr, vals, trigger_info);
        c.push_snapshot(CALI_SCOPE_PROCESS | CALI_SCOPE_THREAD, &trigger_info);
    }

    void
    handle_context_event(CUcontext context, const Attribute& name_attr, const Variant& v_name)
    {
        uint32_t  context_id = 0;
        uint32_t  device_id  = 0;

        if (cuptiGetDeviceId(context,  &device_id)  != CUPTI_SUCCESS)
            return;
        if (cuptiGetContextId(context, &context_id) != CUPTI_SUCCESS)
            return;

        Attribute attr[3] = { cupti_info.device_attr,
                              cupti_info.context_attr,
                              name_attr };
        Variant   vals[3] = { Variant(static_cast<uint64_t>(device_id)),
                              Variant(static_cast<uint64_t>(context_id)),
                              v_name };

        SnapshotRecord::FixedSnapshotRecord<3> trigger_info_data;
        SnapshotRecord trigger_info(trigger_info_data);

        Caliper c;

        c.make_entrylist(3, attr, vals, trigger_info);
        c.push_snapshot(CALI_SCOPE_PROCESS | CALI_SCOPE_THREAD, &trigger_info);
    }

    void
    handle_resource(CUpti_CallbackIdResource cbid, CUpti_ResourceData* cbInfo)
    {
        ++num_resource_cb;

        switch (cbid) {
        case CUPTI_CBID_RESOURCE_CONTEXT_CREATED:
            if (event_sampling.is_enabled())
                event_sampling.enable_sampling_for_context(cbInfo->context);
            
            handle_context_event(cbInfo->context,
                                 cupti_info.resource_attr,
                                 Variant(CALI_TYPE_STRING, "create_context", 15));
            break;
        case CUPTI_CBID_RESOURCE_CONTEXT_DESTROY_STARTING:
            if (event_sampling.is_enabled())
                event_sampling.disable_sampling_for_context(cbInfo->context);

            handle_context_event(cbInfo->context,
                                 cupti_info.resource_attr,
                                 Variant(CALI_TYPE_STRING, "destroy_context", 16));
            break;
        case CUPTI_CBID_RESOURCE_STREAM_CREATED:
            handle_stream_event(cbInfo->context, cbInfo->resourceHandle.stream,
                                cupti_info.resource_attr,
                                Variant(CALI_TYPE_STRING,  "create_stream", 14));
            break;
        case CUPTI_CBID_RESOURCE_STREAM_DESTROY_STARTING:
            handle_stream_event(cbInfo->context, cbInfo->resourceHandle.stream,
                                cupti_info.resource_attr,
                                Variant(CALI_TYPE_STRING,  "destroy_stream", 15));
            break;
        default:
            ;
        }
    }

    void
    handle_synchronize(CUpti_CallbackIdSync cbid, CUpti_SynchronizeData* cbInfo)
    {
        ++num_sync_cb;

        switch (cbid) {
        case CUPTI_CBID_SYNCHRONIZE_STREAM_SYNCHRONIZED:
            handle_stream_event(cbInfo->context, cbInfo->stream,
                                cupti_info.sync_attr,
                                Variant(CALI_TYPE_STRING, "stream",   7));
            break;

        case CUPTI_CBID_SYNCHRONIZE_CONTEXT_SYNCHRONIZED:
            handle_context_event(cbInfo->context,
                                 cupti_info.sync_attr,
                                 Variant(CALI_TYPE_STRING, "context", 8));
            break;
        default:
            // Do nothing
            ;
        }
    }

    void
    handle_callback(CUpti_CallbackId cbid, CUpti_CallbackData* cbInfo, const Attribute& attr)
    {
        ++num_api_cb;

        Caliper c;

        // --- Don't record context id for now: need better way to pass this info through
        // if (cupti_info.record_context) {
        //     uint64_t ctx = cbInfo->contextUid;
        //     Entry    e   = c.get(cupti_info.context_attr);

        //     if (e.is_empty() || e.value().to_uint() != ctx)
        //         c.set(cupti_info.context_attr, Variant(ctx));
        // }

        if (cbInfo->callbackSite == CUPTI_API_ENTER) {
            if (cupti_info.record_symbol && cbInfo->symbolName) {
                Variant v_sname(CALI_TYPE_STRING, cbInfo->symbolName, strlen(cbInfo->symbolName));
                c.set(cupti_info.symbol_attr, v_sname);
            }

            Variant v_fname(CALI_TYPE_STRING, cbInfo->functionName, strlen(cbInfo->functionName));
            c.begin(attr, v_fname);
        } else if (cbInfo->callbackSite == CUPTI_API_EXIT) {
            c.end(attr);

            if (cupti_info.record_symbol && cbInfo->symbolName)
                c.end(cupti_info.symbol_attr);
        }
    }

    void
    handle_nvtx(CUpti_CallbackId cbid, CUpti_NvtxData* cbInfo)
    {
        ++num_nvtx_cb;

        const void* p = cbInfo->functionParams;

        switch (cbid) {
        case CUPTI_CBID_NVTX_nvtxRangePushA:
        {
            const char* msg =
                static_cast<const nvtxRangePushA_params*>(p)->message;

            Caliper().begin(cupti_info.nvtx_range_attr,
                            Variant(CALI_TYPE_STRING, msg, strlen(msg)+1));
        }
        break;
        case CUPTI_CBID_NVTX_nvtxRangePushEx:
        {
            const char* msg =
                static_cast<const nvtxRangePushEx_params*>(p)->eventAttrib->message.ascii;

            Caliper().begin(cupti_info.nvtx_range_attr,
                            Variant(CALI_TYPE_STRING, msg, strlen(msg)+1));
        }
        break;
        case CUPTI_CBID_NVTX_nvtxRangePop:
            Caliper().end(cupti_info.nvtx_range_attr);
            break;
        case CUPTI_CBID_NVTX_nvtxDomainRangePushEx:
        {
            // TODO: Use domain-specific attribute

            const char* msg =
                static_cast<const nvtxDomainRangePushEx_params*>(p)->core.eventAttrib->message.ascii;

            Caliper().begin(cupti_info.nvtx_range_attr,
                            Variant(CALI_TYPE_STRING, msg, strlen(msg)+1));
        }
        break;
        case CUPTI_CBID_NVTX_nvtxDomainRangePop:
            // TODO: Use domain-specific attribute
            Caliper().end(cupti_info.nvtx_range_attr);
            break;
        default:
            ;
        }
    }

    void CUPTIAPI
    cupti_callback(void* userdata,
                   CUpti_CallbackDomain domain,
                   CUpti_CallbackId     cbid,
                   void* cbInfo)
    {
        ++num_cb;

        switch (domain) {
        case CUPTI_CB_DOMAIN_RESOURCE:
            handle_resource(static_cast<CUpti_CallbackIdResource>(cbid),
                            static_cast<CUpti_ResourceData*>(cbInfo));
            break;
        case CUPTI_CB_DOMAIN_SYNCHRONIZE:
            handle_synchronize(static_cast<CUpti_CallbackIdSync>(cbid),
                               static_cast<CUpti_SynchronizeData*>(cbInfo));
            break;
        case CUPTI_CB_DOMAIN_RUNTIME_API:
            handle_callback(cbid, static_cast<CUpti_CallbackData*>(cbInfo),
                            cupti_info.runtime_attr);
            break;
        case CUPTI_CB_DOMAIN_DRIVER_API:
            handle_callback(cbid, static_cast<CUpti_CallbackData*>(cbInfo),
                            cupti_info.driver_attr);
            break;
        case CUPTI_CB_DOMAIN_NVTX:
            handle_nvtx(cbid, static_cast<CUpti_NvtxData*>(cbInfo));
            break;
        default:
            Log(2).stream() << "cupti: Unknown callback domain " << domain << std::endl;
        }

        return;
    }

    void
    snapshot_cb(Caliper* c, int /* scope */, const SnapshotRecord* trigger_info, SnapshotRecord* snapshot)
    {
        event_sampling.snapshot(c, trigger_info, snapshot);
    }
    
    void
    finish_cb(Caliper* c)
    {
        if (Log::verbosity() >= 2) {
            Log(2).stream() << "Cupti: processed "
                            << num_api_cb      << " API callbacks, "
                            << num_resource_cb << " resource callbacks, "
                            << num_sync_cb     << " sync callbacks, "
                            << num_nvtx_cb     << " nvtx callbacks ("
                            << num_cb          << " total)."
                            << std::endl;

            if (event_sampling.is_enabled())
                event_sampling.print_statistics(Log(2).stream());
        }
        
        event_sampling.stop_all();
        
        cuptiUnsubscribe(subscriber);
        cuptiFinalize();
    }

    void
    post_init_cb(Caliper* c)
    {
        //   Need to create attributes in post_init so they're created after
        // the event service.

        Variant v_true(true);

        cupti_info.runtime_attr =
            c->create_attribute("cupti.runtimeAPI", CALI_TYPE_STRING, CALI_ATTR_NESTED);
        cupti_info.driver_attr =
            c->create_attribute("cupti.driverAPI",  CALI_TYPE_STRING, CALI_ATTR_NESTED);
        cupti_info.resource_attr =
            c->create_attribute("cupti.resource",   CALI_TYPE_STRING, CALI_ATTR_DEFAULT);
        cupti_info.sync_attr =
            c->create_attribute("cupti.sync",       CALI_TYPE_STRING, CALI_ATTR_DEFAULT);
        cupti_info.nvtx_range_attr =
            c->create_attribute("nvtx.range",       CALI_TYPE_STRING, CALI_ATTR_NESTED);

        cupti_info.context_attr =
            c->create_attribute("cupti.contextID",  CALI_TYPE_UINT,   CALI_ATTR_SKIP_EVENTS);
        cupti_info.symbol_attr =
            c->create_attribute("cupti.symbolName", CALI_TYPE_STRING, CALI_ATTR_SKIP_EVENTS);
        cupti_info.device_attr =
            c->create_attribute("cupti.deviceID",   CALI_TYPE_UINT,   CALI_ATTR_SKIP_EVENTS);
        cupti_info.stream_attr =
            c->create_attribute("cupti.streamID",   CALI_TYPE_UINT,   CALI_ATTR_SKIP_EVENTS);
    }

    bool
    register_callback_domains()
    {
        CUptiResult res =
            cuptiSubscribe(&subscriber,
                           (CUpti_CallbackFunc) cupti_callback,
                           &cupti_info);

        if (res != CUPTI_SUCCESS) {
            print_cupti_error(Log(0).stream(), res, "cuptiSubscribe");
            return false;
        }

        std::vector<std::string> cb_domain_names =
            config.get("callback_domains").to_stringlist(",:");

        // add "resource" domain when event sampling is enabled 
        if (event_sampling.is_enabled() &&
            std::find(cb_domain_names.begin(), cb_domain_names.end(),
                      "resource") == cb_domain_names.end()) {
            Log(1).stream() << "cupti: Event sampling requires resource callbacks, "
                "adding \"resource\" callback domain."
                            << std::endl;

            cb_domain_names.push_back("resource");
        }

        for (const std::string& s : cb_domain_names) {
            const CallbackDomainInfo* cbinfo = callback_domains;

            for ( ; cbinfo->name && s != cbinfo->name; ++cbinfo)
                ;

            if (!cbinfo->name) {
                Log(0).stream() << "cupti: warning: Unknown callback domain \""
                                << s << "\"" << std::endl;
                continue;
            }

            if (cbinfo->domain != CUPTI_CB_DOMAIN_INVALID) {
                res = cuptiEnableDomain(1, subscriber, cbinfo->domain);

                if (res != CUPTI_SUCCESS) {
                    print_cupti_error(Log(0).stream(), res, "cuptiEnableDomain");
                    return false;
                }

                Log(2).stream() << "cupti: enabled \""
                                << cbinfo->name << "\" callback domain."
                                << std::endl;
            }
        }

        return true;
    }

    void
    cuptiservice_initialize(Caliper* c)
    {
        config = RuntimeConfig::init("cupti", configdata);

        num_cb          = 0;
        num_api_cb      = 0;
        num_resource_cb = 0;
        num_sync_cb     = 0;
        num_nvtx_cb     = 0;

        uint64_t sample_event_id = config.get("sample_event_id").to_uint();

        if (sample_event_id > 0)
            event_sampling.setup(c, static_cast<CUpti_EventID>(sample_event_id));
        
        if (!register_callback_domains())
            return;

        cupti_info.record_context = config.get("record_context").to_bool();
        cupti_info.record_symbol  = config.get("record_symbol").to_bool();

        if (event_sampling.is_enabled())
            c->events().snapshot.connect(&snapshot_cb);

        c->events().post_init_evt.connect(&post_init_cb);
        c->events().finish_evt.connect(&finish_cb);

        Log(1).stream() << "Registered cupti service" << std::endl;
    }

} // namespace

namespace cali
{
    CaliperService cupti_service = { "cupti", ::cuptiservice_initialize };
}
