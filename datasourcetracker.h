/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef __Datasourcetracker_H__
#define __Datasourcetracker_H__

#include "config.h"

#include <pthread.h>
#include <string>
#include <vector>
#include <map>

#include "globalregistry.h"
#include "util.h"
#include "kis_datasource.h"
#include "trackedelement.h"
#include "kis_net_microhttpd.h"
#include "entrytracker.h"
#include "timetracker.h"

/* Data source tracker
 *
 * Core of the new capture management system.
 *
 * This code replaces the old packetsource tracker.
 *
 * Data sources are registered passing a builder instance which is used to
 * instantiate the final versions of the data sources.  
 *
 * Data sources communicate via the protocol defined in simple_cap_proto.h 
 * and may communicate packets or complete device objects.
 *
 * 'Auto' type sources (sources with type=auto or no type given) are 
 * probed automatically via all the registered datasource drivers.  
 * Datasource drivers may require starting a process in order to perform the
 * probe, or they may be able to perform the probe in C++ native code.
 *
 * Once a source driver is found, it is instantiated as an active source and
 * put in the list of sources.  Opening the source may result in an error, 
 * but as the source is actually assigned, it will remain in the source list.
 * This is to allow defining sources that may not be plugged in yet, etc.
 *
 * Devices which encounter errors are placed in the error vector and 
 * periodically re-tried
 *
 */

class Datasourcetracker;
class KisDataSource;
class DST_Worker;

// Worker class used to perform work on the list of packet-sources in a thread
// safe / continuity safe context.
class DST_Worker {
public:
    DST_Worker() { };

    // Handle a data source when working on iterate_datasources
    virtual void handle_datasource(shared_ptr<KisDataSource> in_src __attribute__((unused))) { };

    // All data sources have been processed in iterate_datasources
    virtual void finalize() { };
};

// Datasource prototype for easy tracking and exporting
class DST_DataSourcePrototype : public tracker_component {
public:
    DST_DataSourcePrototype(GlobalRegistry *in_globalreg);

    shared_ptr<KisDataSource> get_proto_builder() { return proto_builder; }
    void set_proto_builder(shared_ptr<KisDataSource> in_builder);

    __Proxy(proto_type, string, string, string, proto_type);
    __Proxy(proto_description, string, string, string, proto_description);

protected:
    GlobalRegistry *globalreg;

    virtual void register_fields();

    int proto_type_id;
    SharedTrackerElement proto_type;

    int proto_description_id;
    SharedTrackerElement proto_description;

    // Builder used for probe and building the valid source
    shared_ptr<KisDataSource> proto_builder;
};

// Probing record, generated to keep track of source responses during type probe.
// Used as the auxptr for the probe callback.
//
// * Source added with 'auto' type
// * All current sources instantiated in probe mode
// * Probe called on each source with DST probe handler as the callback
// * As probe responses come in, delete the probe instance of the source
// * If a positive probe response comes in, remove handlers from all other probes and
//   cancel the probes for the rest
class DST_DataSourceProbe {
public:
    DST_DataSourceProbe(time_t in_time, string in_definition, 
            shared_ptr<Datasourcetracker> in_tracker, 
            vector<shared_ptr<KisDataSource> > in_protovec);
    virtual ~DST_DataSourceProbe();

    time_t get_time() { return start_time; }
    shared_ptr<Datasourcetracker> get_tracker() { return tracker; }
    string get_definition() { return definition; }

    shared_ptr<KisDataSource> get_proto();
    void set_proto(shared_ptr<KisDataSource> in_proto);

    // Clear a source from the list, returns number of sources left in the list.  Used
    // to purge failures out of the probe list and know when we've finished
    size_t remove_failed_proto(shared_ptr<KisDataSource> in_src);

    void cancel();

protected:
    pthread_mutex_t probe_lock;

    shared_ptr<Datasourcetracker> tracker;

    // Vector of sources we're still waiting to return from probing
    vector<shared_ptr<KisDataSource> > protosrc_vec;

    // Source we've found
    shared_ptr<KisDataSource> protosrc;

    time_t start_time;
    string definition;
};

class Datasourcetracker : public Kis_Net_Httpd_Stream_Handler, 
    public TimetrackerEvent, public LifetimeGlobal {
public:
    static shared_ptr<Datasourcetracker> create_dst(GlobalRegistry *in_globalreg) {
        shared_ptr<Datasourcetracker> mon(new Datasourcetracker(in_globalreg));
        in_globalreg->RegisterLifetimeGlobal(mon);
        in_globalreg->InsertGlobal("DATA_SOURCE_TRACKER", mon);
        return mon;
    }

private:
    Datasourcetracker(GlobalRegistry *in_globalreg);

public:
    virtual ~Datasourcetracker();

    // Add a datasource builder, with type and description.  Returns 0 or positive on
    // success, negative on failure
    int register_datasource_builder(string in_type, string in_description,
            shared_ptr<KisDataSource> in_builder);

    // Operate on all data sources currently defined.  The datasource tracker is locked
    // during this operation, making it thread safe.
    void iterate_datasources(DST_Worker *in_worker);

    // Launch a source.  If there is no type defined or the type is 'auto', attempt to
    // find the source.  When the source is opened or there is a failure, 
    // in_open_handler will be called.
    //
    // Opening a data source is an asynchronous operation - the worker will be called
    // at some point in the future.  Callers requiring a blocking operation should call
    // this in a dedicated thread and wait for the thread to re-join.
    //
    // Malformed source definitions will result in an immediate error & failure callback
    // of the worker.  All other sources will result in an immediate success and async
    // callback of the worker for final success.
    int open_datasource(string in_source);

    // Remove a data source
    bool remove_datasource(uuid in_uud);

    // HTTP api
    virtual bool Httpd_VerifyPath(const char *path, const char *method);

    virtual void Httpd_CreateStreamResponse(Kis_Net_Httpd *httpd,
            Kis_Net_Httpd_Connection *connection,
            const char *url, const char *method, const char *upload_data,
            size_t *upload_data_size, std::stringstream &stream);

    virtual int Httpd_PostIterator(void *coninfo_cls, enum MHD_ValueKind kind, 
            const char *key, const char *filename, const char *content_type,
            const char *transfer_encoding, const char *data, 
            uint64_t off, size_t size);

    // Timetracker API
    virtual int timetracker_event(int eventid);

protected:
    GlobalRegistry *globalreg;

    shared_ptr<EntryTracker> entrytracker;

    pthread_mutex_t dst_lock;

    int error_timer_id;

    int dst_proto_entry_id;
    int dst_source_entry_id;

    // Lists of proto and active sources
    SharedTrackerElement proto_vec;
    SharedTrackerElement datasource_vec;
    SharedTrackerElement error_vec;

    // Currently probing
    vector<shared_ptr<DST_DataSourceProbe> > probing_vec;

    // Start a probe for finding a source to handle the auto type
    void start_source_probe(string in_source);

    // Callbacks for source async operations
    static void probe_handler(shared_ptr<KisDataSource> in_src, 
            shared_ptr<void> in_aux, bool in_success);
    static void open_handler(shared_ptr<KisDataSource> in_src, 
            shared_ptr<void> in_aux, bool in_success);
    static void error_handler(shared_ptr<KisDataSource> in_src, 
            shared_ptr<void> in_aux);

    // Initiate a source from a known proto, add it to the list of open sources,
    // and report success via the worker.  PERFORMS THREAD LOCK, do NOT call
    // inside of a locked thread
    void launch_source(shared_ptr<KisDataSource> in_proto, string in_source);
};


#endif

