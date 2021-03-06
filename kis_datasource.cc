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

#include "config.h"

#include "kis_datasource.h"
#include "simple_datasource_proto.h"
#include "endian_magic.h"
#include "configfile.h"
#include "msgpack_adapter.h"

#ifdef HAVE_LIBPCRE
#include <pcre.h>
#endif

KisDataSource::KisDataSource(GlobalRegistry *in_globalreg) :
    tracker_component(in_globalreg, 0) {
    globalreg = in_globalreg;

    packetchain = 
        static_pointer_cast<Packetchain>(globalreg->FetchGlobal("PACKETCHAIN"));

	pack_comp_linkframe = packetchain->RegisterPacketComponent("LINKFRAME");
    pack_comp_l1info = packetchain->RegisterPacketComponent("RADIODATA");
    pack_comp_gps = packetchain->RegisterPacketComponent("GPS");

    pthread_mutex_init(&source_lock, NULL);

    probe_callback = NULL;
    probe_aux = NULL;

    error_callback = NULL;
    error_aux = NULL;

    open_callback = NULL;
    open_aux = NULL;

    register_fields();
    reserve_fields(NULL);

    set_source_running(false);

    ipchandler = NULL;
    source_ipc = NULL;
}

KisDataSource::~KisDataSource() {
    close_source();

    {
        // Make sure no-one is holding a reference to us
        local_locker lock(&source_lock);
    }

    pthread_mutex_destroy(&source_lock);
}

void KisDataSource::close_source() {
    cancel_probe_source();
    cancel_open_source();

    if (source_ipc != NULL) {
        source_ipc->close_ipc();
        source_ipc->soft_kill();
    }

    set_source_running(false);
    set_child_pid(-1);
}

void KisDataSource::register_fields() {
    source_name_id =
        RegisterField("kismet.datasource.source_name", TrackerString,
                "Human name of data source", &source_name);
    source_type_id =
        RegisterField("kismet.datasource.source_type", TrackerString,
                "Type of data source", &source_type);
    source_interface_id =
        RegisterField("kismet.datasource.source_interface", TrackerString,
                "Primary capture interface", &source_interface);
    source_uuid_id =
        RegisterField("kismet.datasource.source_uuid", TrackerUuid,
                "UUID", &source_uuid);
    source_id_id =
        RegisterField("kismet.datasource.source_id", TrackerInt32,
                "Run-time ID", &source_id);
    source_channel_capable_id =
        RegisterField("kismet.datasource.source_channel_capable", TrackerUInt8,
                "(bool) source capable of channel change", 
                &source_channel_capable);
    child_pid_id =
        RegisterField("kismet.datasource.child_pid", TrackerInt64,
                "PID of data capture process", &child_pid);
    source_definition_id =
        RegisterField("kismet.datasource.definition", TrackerString,
                "original source definition", &source_definition);
    source_description_id =
        RegisterField("kismet.datasource.description", TrackerString,
                "human-readable description", &source_description);

    source_channel_entry_id =
        globalreg->entrytracker->RegisterField("kismet.device.base.channel", 
                TrackerString, "channel (phy specific)");
    source_channels_vec_id =
        RegisterField("kismet.datasource.channels", TrackerVector,
                "valid channels for this device", &source_channels_vec);

    ipc_errors_id =
        RegisterField("kismet.datasource.ipc_errors", TrackerUInt64,
                "number of errors in IPC protocol", &ipc_errors);
    source_running_id =
        RegisterField("kismet.datasource.running", TrackerUInt8,
                "source is currently operational", &source_running);
    source_hopping_id = 
        RegisterField("kismet.datasource.hopping", TrackerUInt8,
                "source is channel hopping (bool)", &source_hopping);
    source_hop_rate_id =
        RegisterField("kismet.datasource.hop_rate", TrackerDouble,
                "channel hopping rate", &source_hop_rate);
    source_hop_vec_id =
        RegisterField("kismet.datasource.hop_channels", TrackerVector,
                "hopping channels", &source_hop_vec);
    source_ipc_bin_id =
        RegisterField("kismet.datasource.ipc_bin", TrackerString,
                "driver binary", &source_ipc_bin);

    last_report_time_id =
        RegisterField("kismet.datasource.last_report_time", TrackerUInt64,
                "last packet/device report time", 
                &last_report_time);

    num_reports_id =
        RegisterField("kismet.datasource.num_reports", TrackerUInt64,
                "number of packtes/device reports", &num_reports);
}

void KisDataSource::BufferAvailable(size_t in_amt) {
    simple_cap_proto_t *frame_header;
    uint8_t *buf;
    uint32_t frame_sz;
    uint32_t frame_checksum, calc_checksum;

    if (in_amt < sizeof(simple_cap_proto_t)) {
        return;
    }

    // Peek the buffer
    buf = new uint8_t[in_amt];
    ipchandler->PeekReadBufferData(buf, in_amt);

    frame_header = (simple_cap_proto_t *) buf;

    if (kis_ntoh32(frame_header->signature) != KIS_CAP_SIMPLE_PROTO_SIG) {
        // TODO kill connection or seek for valid
        delete[] buf;
        return;
    }

    frame_sz = kis_ntoh32(frame_header->packet_sz);

    if (frame_sz > in_amt) {
        // Nothing we can do right now, not enough data to make up a
        // complete packet.
        delete[] buf;
        return;
    }

    // Get the checksum
    frame_checksum = kis_ntoh32(frame_header->checksum);

    // Zero the checksum field in the packet
    frame_header->checksum = 0x00000000;

    // Calc the checksum of the rest
    calc_checksum = Adler32Checksum((const char *) buf, frame_sz);

    // Compare to the saved checksum
    if (calc_checksum != frame_checksum) {
        // TODO report invalid checksum and disconnect
        delete[] buf;
        return;
    }

    // Consume the packet in the ringbuf 
    ipchandler->GetReadBufferData(NULL, frame_sz);

    // Extract the kv pairs
    KVmap kv_map;

    ssize_t data_offt = 0;
    for (unsigned int kvn = 0; kvn < kis_ntoh32(frame_header->num_kv_pairs); kvn++) {
        simple_cap_proto_kv *pkv =
            (simple_cap_proto_kv *) &((frame_header->data)[data_offt]);

        data_offt = 
            sizeof(simple_cap_proto_kv_h_t) +
            kis_ntoh32(pkv->header.obj_sz);

        KisDataSource_CapKeyedObject *kv =
            new KisDataSource_CapKeyedObject(pkv);

        kv_map[StrLower(kv->key)] = kv;
    }

    char ctype[17];
    snprintf(ctype, 17, "%s", frame_header->type);
    handle_packet(ctype, kv_map);

    for (KVmap::iterator i = kv_map.begin(); i != kv_map.end(); ++i) {
        delete i->second;
    }

    delete[] buf;

}

void KisDataSource::BufferError(string in_error) {
    _MSG(in_error, MSGFLAG_ERROR);
    
    {
        local_locker lock(&source_lock);

        // Trip all the callbacks
        if (probe_callback != NULL) {
            (*probe_callback)(shared_ptr<KisDataSource>(this), probe_aux, false);
        }

        if (open_callback != NULL) {
            (*probe_callback)(shared_ptr<KisDataSource>(this), probe_aux, false);
        }

        if (error_callback != NULL) {
            (*error_callback)(shared_ptr<KisDataSource>(this), error_aux);
        }

        // Kill the IPC
        source_ipc->soft_kill();

        set_source_running(false);
        set_child_pid(0);

    }
}

bool KisDataSource::queue_ipc_command(string in_cmd, KVmap *in_kvpairs) {

    // If IPC is running just write it straight out
    if (source_ipc != NULL && source_ipc->get_pid() > 0) {
        bool ret = false;

        ret = write_ipc_packet(in_cmd, in_kvpairs);

        if (ret) {
            for (KVmap::iterator i = in_kvpairs->begin(); i != in_kvpairs->end(); ++i) {
                delete i->second;
            }
            delete in_kvpairs;

            return ret;
        }
    }

    // If we didn't succeed in writing the packet for some reason

    // Queue the command
    KisDataSource_QueuedCommand *cmd = 
        new KisDataSource_QueuedCommand(in_cmd, in_kvpairs, 
                globalreg->timestamp.tv_sec);

    {
        local_locker lock(&source_lock);
        pending_commands.push_back(cmd);
    }

    return true;
}

bool KisDataSource::write_ipc_packet(string in_type, KVmap *in_kvpairs) {
    simple_cap_proto_t *ret = NULL;
    vector<simple_cap_proto_kv_t *> proto_kvpairs;
    size_t kvpair_len = 0;
    size_t kvpair_offt = 0;
    size_t pack_len;

    for (KVmap::iterator i = in_kvpairs->begin(); i != in_kvpairs->end(); ++i) {
        // Size of header + size of object
        simple_cap_proto_kv_t *kvt = (simple_cap_proto_kv_t *) 
            new char[sizeof(simple_cap_proto_kv_h_t) + i->second->size];

        // Set up the header, network endian
        snprintf(kvt->header.key, 16, "%s", i->second->key.c_str());
        kvt->header.obj_sz = kis_hton32(i->second->size);

        // Copy the content
        memcpy(kvt->object, i->second->object, i->second->size);

        // Add the total size
        kvpair_len += sizeof(simple_cap_proto_kv_h_t) + i->second->size;
    }

    // Make the container packet
    pack_len = sizeof(simple_cap_proto_t) + kvpair_len;

    ret = (simple_cap_proto_t *) new char[pack_len];

    ret->signature = kis_hton32(KIS_CAP_SIMPLE_PROTO_SIG);
   
    // Prep the checksum with 0
    ret->checksum = 0;

    ret->packet_sz = kis_hton32(pack_len);

    snprintf(ret->type, 16, "%s", in_type.c_str());

    ret->num_kv_pairs = kis_hton32(proto_kvpairs.size());

    // Progress through the kv pairs and pack them 
    for (unsigned int i = 0; i < proto_kvpairs.size(); i++) {
        // Annoying to have to do it this way
        size_t len = kis_ntoh32(proto_kvpairs[i]->header.obj_sz) +
            sizeof(simple_cap_proto_kv_h_t);

        memcpy(&(ret->data[kvpair_offt]), proto_kvpairs[i], len);

        kvpair_offt += len;

        // Delete it as we go
        delete(proto_kvpairs[i]);
    }

    // Calculate the checksum with it pre-populated as 0x0
    uint32_t calc_checksum;
    calc_checksum = Adler32Checksum((const char *) ret, pack_len);

    ret->checksum = kis_hton32(calc_checksum);

    size_t ret_sz;

    {
        // Lock & send to the IPC ringbuffer
        local_locker lock(&source_lock);
        ret_sz = ipchandler->PutWriteBufferData(ret, pack_len, true);

        delete ret;
    }

    if (ret_sz != pack_len)
        return false;

    return true;
}

void KisDataSource::set_error_handler(error_handler in_cb, shared_ptr<void> in_aux) {
    local_locker lock(&source_lock);

    error_callback = in_cb;
    error_aux = in_aux;
}

void KisDataSource::cancel_error_handler() {
    local_locker lock(&source_lock);

    error_callback = NULL;
    error_aux = NULL;
}

bool KisDataSource::probe_source(string in_source, probe_handler in_cb,
        shared_ptr<void> in_aux) {
    local_locker lock(&source_lock);

    // Fail out an existing callback
    if (probe_callback != NULL) {
        (*probe_callback)(shared_ptr<KisDataSource>(this), probe_aux, false);
    }

    if (!spawn_ipc()) {
        if (in_cb != NULL) {
            (*in_cb)(shared_ptr<KisDataSource>(this), in_aux, false);
        }

        if (error_callback != NULL) {
            (*error_callback)(shared_ptr<KisDataSource>(this), error_aux);
        }

        return false;
    }

    return true;
}

bool KisDataSource::open_source(string in_definition, open_handler in_cb, 
        shared_ptr<void> in_aux) {
    local_locker lock(&source_lock);

    // Fail out any existing callback
    if (open_callback != NULL) {
        (*open_callback)(shared_ptr<KisDataSource>(this), in_aux, false);
    }

    // Set the callback to get run when we get the openresp
    open_callback = in_cb;
    open_aux = in_aux;

    set_source_definition(in_definition);

    // Launch the IPC, fail immediately if we can't
    if (!spawn_ipc()) {
        if (in_cb != NULL) {
            (*in_cb)(shared_ptr<KisDataSource>(this), in_aux, false);
        }

        if (error_callback != NULL) {
            (*error_callback)(shared_ptr<KisDataSource>(this), error_aux);
        }

        return false;
    }

    KisDataSource_CapKeyedObject *definition =
        new KisDataSource_CapKeyedObject("DEFINITION", in_definition.data(),
                in_definition.length());
    KVmap *kvmap = new KVmap();

    kvmap->insert(KVpair("DEFINITION", definition));

    queue_ipc_command("OPENDEVICE", kvmap);

    return true;
}

void KisDataSource::cancel_probe_source() {
    local_locker lock(&source_lock);

    probe_callback = NULL;
    probe_aux = NULL;
}

void KisDataSource::cancel_open_source() {
    local_locker lock(&source_lock);

    open_callback = NULL;
    open_aux = NULL;
}

void KisDataSource::set_channel(string in_channel) {
    if (source_ipc == NULL) {
        _MSG("Attempt to set channel on source which is closed", MSGFLAG_ERROR);
        return;
    }

    if (source_ipc->get_pid() <= 0) {
        _MSG("Attempt to set channel on source with closed IPC", MSGFLAG_ERROR);
        return;
    }

    KisDataSource_CapKeyedObject *chanset =
        new KisDataSource_CapKeyedObject("CHANSET", in_channel.data(),
                in_channel.length());
    KVmap *kvmap = new KVmap();

    kvmap->insert(KVpair("CHANSET", chanset));

    queue_ipc_command("CONFIGURE", kvmap);
}

void KisDataSource::set_channel_hop(vector<string> in_channel_list, 
        double in_rate) {

    if (source_ipc == NULL) {
        _MSG("Attempt to set channel hop on source which is closed", MSGFLAG_ERROR);
        return;
    }

    if (source_ipc->get_pid() <= 0) {
        _MSG("Attempt to set channel hop on source with closed IPC", MSGFLAG_ERROR);
        return;
    }

    stringstream stream;
    msgpack::packer<std::stringstream> packer(&stream);

    // 2-element dictionary
    packer.pack_map(2);

    // Pack the rate dictionary entry
    packer.pack(string("rate"));
    packer.pack(in_rate);

    // Pack the vector of channels
    packer.pack(string("channels"));
    packer.pack_array(in_channel_list.size());

    for (vector<string>::iterator i = in_channel_list.begin();
            i != in_channel_list.end(); ++i) {
        packer.pack(*i);
    }

    KisDataSource_CapKeyedObject *chanhop =
        new KisDataSource_CapKeyedObject("CHANHOP", stream.str().data(),
                stream.str().length());
    KVmap *kvmap = new KVmap();

    kvmap->insert(KVpair("CHANHOP", chanhop));

    queue_ipc_command("CONFIGURE", kvmap);
}

void KisDataSource::handle_packet(string in_type, KVmap in_kvmap) {
    string ltype = StrLower(in_type);

    if (ltype == "status")
        handle_packet_status(in_kvmap);
    else if (ltype == "proberesp")
        handle_packet_probe_resp(in_kvmap);
    else if (ltype == "openresp")
        handle_packet_open_resp(in_kvmap);
    else if (ltype == "error")
        handle_packet_error(in_kvmap);
    else if (ltype == "message")
        handle_packet_message(in_kvmap);
    else if (ltype == "data")
        handle_packet_data(in_kvmap);
}

void KisDataSource::handle_packet_status(KVmap in_kvpairs) {
    KVmap::iterator i;
    
    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        handle_kv_message(i->second);
    }

    // If we just launched, this lets us know we're awake and can 
    // send any queued commands

}

void KisDataSource::handle_packet_probe_resp(KVmap in_kvpairs) {
    KVmap::iterator i;

    // Process any messages
    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        handle_kv_message(i->second);
    }

    // Process channels list if we got one
    if ((i = in_kvpairs.find("channels")) != in_kvpairs.end()) {
        if (!handle_kv_channels(i->second))
            return;
    }

    // Process success value and callback
    if ((i = in_kvpairs.find("success")) != in_kvpairs.end()) {
        local_locker lock(&source_lock);

        if (probe_callback != NULL) {
            (*probe_callback)(shared_ptr<KisDataSource>(this), probe_aux, 
                    handle_kv_success(i->second));
        }
    } else {
        // ProbeResp with no success value?  ehh.
        local_locker lock(&source_lock);
        inc_ipc_errors(1);
        return;
    }

    // Close the source since probe is done
    source_ipc->close_ipc();
}

void KisDataSource::handle_packet_open_resp(KVmap in_kvpairs) {
    KVmap::iterator i;

    // Process any messages
    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        handle_kv_message(i->second);
    }

    // Process channels list if we got one
    if ((i = in_kvpairs.find("channels")) != in_kvpairs.end()) {
        if (!handle_kv_channels(i->second))
            return;
    }

    // Process success value and callback
    if ((i = in_kvpairs.find("success")) != in_kvpairs.end()) {
        local_locker lock(&source_lock);

        if (open_callback != NULL) {
            (*open_callback)(shared_ptr<KisDataSource>(this), 
                    open_aux, handle_kv_success(i->second));
        }
    } else {
        // OpenResp with no success value?  ehh.
        local_locker lock(&source_lock);
        inc_ipc_errors(1);
        return;
    }

}

void KisDataSource::handle_packet_error(KVmap in_kvpairs) {
    KVmap::iterator i;

    // Process any messages
    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        handle_kv_message(i->second);
    }

    // Lock only after handling messages
    {
        local_locker lock(&source_lock);

        // Kill the IPC
        source_ipc->soft_kill();

        set_source_running(false);
        set_child_pid(0);

        if (error_callback != NULL)
            (*error_callback)(shared_ptr<KisDataSource>(this), error_aux);
    }
}


void KisDataSource::handle_packet_message(KVmap in_kvpairs) {
    KVmap::iterator i;

    // Process any messages
    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        handle_kv_message(i->second);
    }
}

void KisDataSource::handle_packet_data(KVmap in_kvpairs) {
    KVmap::iterator i;

    kis_packet *packet = NULL;
    kis_layer1_packinfo *siginfo = NULL;
    kis_gps_packinfo *gpsinfo = NULL;

    // Process any messages
    if ((i = in_kvpairs.find("message")) != in_kvpairs.end()) {
        handle_kv_message(i->second);
    }

    // Do we have a packet?
    if ((i = in_kvpairs.find("packet")) != in_kvpairs.end()) {
        packet = handle_kv_packet(i->second);
    }

    if (packet == NULL)
        return;

    // Gather signal data
    if ((i = in_kvpairs.find("signal")) != in_kvpairs.end()) {
        siginfo = handle_kv_signal(i->second);
    }
    
    // Gather GPS data
    if ((i = in_kvpairs.find("gps")) != in_kvpairs.end()) {
        gpsinfo = handle_kv_gps(i->second);
    }

    // Add them to the packet
    if (siginfo != NULL) {
        packet->insert(pack_comp_l1info, siginfo);
    }

    if (gpsinfo != NULL) {
        packet->insert(pack_comp_gps, gpsinfo);
    }

    // Update the last valid report time
    inc_num_reports(1);
    set_last_report_time(globalreg->timestamp.tv_sec);
    
    // Inject the packet into the packetchain if we have one
    packetchain->ProcessPacket(packet);

}

bool KisDataSource::handle_kv_success(KisDataSource_CapKeyedObject *in_obj) {
    // Not a msgpacked object, just a single byte
    if (in_obj->size != 1) {
        local_locker lock(&source_lock);
        inc_ipc_errors(1);
        return false;
    }

    return in_obj->object[0];
}

bool KisDataSource::handle_kv_message(KisDataSource_CapKeyedObject *in_obj) {
    // Unpack the dictionary
    MsgpackAdapter::MsgpackStrMap dict;
    msgpack::unpacked result;
    MsgpackAdapter::MsgpackStrMap::iterator obj_iter;
    vector<string> channel_vec;

    try {
        msgpack::unpack(result, in_obj->object, in_obj->size); 
        msgpack::object deserialized = result.get();
        dict = deserialized.as<MsgpackAdapter::MsgpackStrMap>();

        string msg;
        unsigned int flags;

        if ((obj_iter = dict.find("msg")) != dict.end()) {
            msg = obj_iter->second.as<string>();
        } else {
            throw std::runtime_error("missing 'msg' entry");
        }

        if ((obj_iter = dict.find("flags")) != dict.end()) {
            flags = obj_iter->second.as<unsigned int>();
        } else {
            throw std::runtime_error("missing 'flags' entry");
        }

        _MSG(msg, flags);

    } catch (const std::exception& e) {
        // Something went wrong with msgpack unpacking
        stringstream ss;
        ss << "Source " << get_source_name() << " failed to unpack message " <<
            "bundle: " << e.what();
        _MSG(ss.str(), MSGFLAG_ERROR);

        local_locker lock(&source_lock);
        inc_ipc_errors(1);

        return false;
    }

    return true;

}

bool KisDataSource::handle_kv_channels(KisDataSource_CapKeyedObject *in_obj) {
    // Unpack the dictionary
    MsgpackAdapter::MsgpackStrMap dict;
    msgpack::unpacked result;
    MsgpackAdapter::MsgpackStrMap::iterator obj_iter;
    vector<string> channel_vec;

    try {
        msgpack::unpack(result, in_obj->object, in_obj->size);
        msgpack::object deserialized = result.get();
        dict = deserialized.as<MsgpackAdapter::MsgpackStrMap>();

        if ((obj_iter = dict.find("channels")) != dict.end()) {
            MsgpackAdapter::AsStringVector(obj_iter->second, channel_vec);

            // We now have a string vector of channels, dupe it into our 
            // tracked channels vec
            local_locker lock(&source_lock);

            source_channels_vec->clear_vector();
            for (unsigned int x = 0; x < channel_vec.size(); x++) {
                SharedTrackerElement chanstr =
                    globalreg->entrytracker->GetTrackedInstance(source_channel_entry_id);
                chanstr->set(channel_vec[x]);
                source_channels_vec->add_vector(chanstr);
            }
        }
    } catch (const std::exception& e) {
        // Something went wrong with msgpack unpacking
        stringstream ss;
        ss << "Source " << get_source_name() << " failed to unpack proberesp " <<
            "channels bundle: " << e.what();
        _MSG(ss.str(), MSGFLAG_ERROR);

        local_locker lock(&source_lock);
        inc_ipc_errors(1);

        return false;
    }

    return true;
}

kis_layer1_packinfo *KisDataSource::handle_kv_signal(KisDataSource_CapKeyedObject *in_obj) {
    kis_layer1_packinfo *siginfo = new kis_layer1_packinfo();

    // Unpack the dictionary
    MsgpackAdapter::MsgpackStrMap dict;
    msgpack::unpacked result;
    MsgpackAdapter::MsgpackStrMap::iterator obj_iter;

    try {
        msgpack::unpack(result, in_obj->object, in_obj->size);
        msgpack::object deserialized = result.get();
        dict = deserialized.as<MsgpackAdapter::MsgpackStrMap>();

        if ((obj_iter = dict.find("signal_dbm")) != dict.end()) {
            siginfo->signal_type = kis_l1_signal_type_dbm;
            siginfo->signal_dbm = obj_iter->second.as<int32_t>();
        }

        if ((obj_iter = dict.find("noise_dbm")) != dict.end()) {
            siginfo->signal_type = kis_l1_signal_type_dbm;
            siginfo->noise_dbm = obj_iter->second.as<int32_t>();
        }

        if ((obj_iter = dict.find("signal_rssi")) != dict.end()) {
            siginfo->signal_type = kis_l1_signal_type_rssi;
            siginfo->signal_rssi = obj_iter->second.as<int32_t>();
        }

        if ((obj_iter = dict.find("noise_rssi")) != dict.end()) {
            siginfo->signal_type = kis_l1_signal_type_rssi;
            siginfo->noise_rssi = obj_iter->second.as<int32_t>();
        }

        if ((obj_iter = dict.find("freq_khz")) != dict.end()) {
            siginfo->freq_khz = obj_iter->second.as<double>();
        }

        if ((obj_iter = dict.find("channel")) != dict.end()) {
            siginfo->channel = obj_iter->second.as<string>();
        }

        if ((obj_iter = dict.find("datarate")) != dict.end()) {
            siginfo->datarate = obj_iter->second.as<double>();
        }

    } catch (const std::exception& e) {
        // Something went wrong with msgpack unpacking
        stringstream ss;
        ss << "Source " << get_source_name() << " failed to unpack gps bundle: " <<
            e.what();
        _MSG(ss.str(), MSGFLAG_ERROR);

        local_locker lock(&source_lock);
        inc_ipc_errors(1);

        delete(siginfo);

        return NULL;
    }


    return siginfo;
}

kis_gps_packinfo *KisDataSource::handle_kv_gps(KisDataSource_CapKeyedObject *in_obj) {
    kis_gps_packinfo *gpsinfo = new kis_gps_packinfo();

    // Unpack the dictionary
    MsgpackAdapter::MsgpackStrMap dict;
    msgpack::unpacked result;
    MsgpackAdapter::MsgpackStrMap::iterator obj_iter;

    try {
        msgpack::unpack(result, in_obj->object, in_obj->size);
        msgpack::object deserialized = result.get();
        dict = deserialized.as<MsgpackAdapter::MsgpackStrMap>();

        if ((obj_iter = dict.find("lat")) != dict.end()) {
            gpsinfo->lat = obj_iter->second.as<double>();
        }

        if ((obj_iter = dict.find("lon")) != dict.end()) {
            gpsinfo->lon = obj_iter->second.as<double>();
        }

        if ((obj_iter = dict.find("alt")) != dict.end()) {
            gpsinfo->alt = obj_iter->second.as<double>();
        }

        if ((obj_iter = dict.find("speed")) != dict.end()) {
            gpsinfo->speed = obj_iter->second.as<double>();
        }

        if ((obj_iter = dict.find("heading")) != dict.end()) {
            gpsinfo->heading = obj_iter->second.as<double>();
        }

        if ((obj_iter = dict.find("precision")) != dict.end()) {
            gpsinfo->precision = obj_iter->second.as<double>();
        }

        if ((obj_iter = dict.find("fix")) != dict.end()) {
            gpsinfo->precision = obj_iter->second.as<int32_t>();
        }

        if ((obj_iter = dict.find("time")) != dict.end()) {
            gpsinfo->time = (time_t) obj_iter->second.as<uint64_t>();
        }

        if ((obj_iter = dict.find("name")) != dict.end()) {
            gpsinfo->gpsname = obj_iter->second.as<string>();
        }

    } catch (const std::exception& e) {
        // Something went wrong with msgpack unpacking
        stringstream ss;
        ss << "Source " << get_source_name() << " failed to unpack gps bundle: " <<
            e.what();
        _MSG(ss.str(), MSGFLAG_ERROR);

        local_locker lock(&source_lock);
        inc_ipc_errors(1);

        delete(gpsinfo);
        return NULL;
    }

    return gpsinfo;

}

kis_packet *KisDataSource::handle_kv_packet(KisDataSource_CapKeyedObject *in_obj) {
    kis_packet *packet = packetchain->GeneratePacket();
    kis_datachunk *datachunk = new kis_datachunk();

    // Unpack the dictionary
    MsgpackAdapter::MsgpackStrMap dict;
    msgpack::unpacked result;
    MsgpackAdapter::MsgpackStrMap::iterator obj_iter;

    try {
        msgpack::unpack(result, in_obj->object, in_obj->size);
        msgpack::object deserialized = result.get();
        dict = deserialized.as<MsgpackAdapter::MsgpackStrMap>();

        if ((obj_iter = dict.find("tv_sec")) != dict.end()) {
            packet->ts.tv_sec = (time_t) obj_iter->second.as<uint64_t>();
        } else {
            throw std::runtime_error(string("tv_sec timestamp missing"));
        }

        if ((obj_iter = dict.find("tv_usec")) != dict.end()) {
            packet->ts.tv_usec = (time_t) obj_iter->second.as<uint64_t>();
        } else {
            throw std::runtime_error(string("tv_usec timestamp missing"));
        }

        if ((obj_iter = dict.find("dlt")) != dict.end()) {
            datachunk->dlt = obj_iter->second.as<uint64_t>();
        } else {
            throw std::runtime_error(string("DLT missing"));
        }

        // Record the size
        uint64_t size = 0;
        if ((obj_iter = dict.find("size")) != dict.end()) {
            size = obj_iter->second.as<uint64_t>();
        } else {
            throw std::runtime_error(string("size field missing or zero"));
        }

        msgpack::object rawdata;
        if ((obj_iter = dict.find("packet")) != dict.end()) {
            rawdata = obj_iter->second;
        } else {
            throw std::runtime_error(string("packet data missing"));
        }

        if (rawdata.via.bin.size != size) {
            throw std::runtime_error(string("packet size did not match data size"));
        }

        datachunk->copy_data((const uint8_t *) rawdata.via.bin.ptr, size);

    } catch (const std::exception& e) {
        // Something went wrong with msgpack unpacking
        stringstream ss;
        ss << "Source " << get_source_name() << " failed to unpack packet bundle: " <<
            e.what();
        _MSG(ss.str(), MSGFLAG_ERROR);

        local_locker lock(&source_lock);
        inc_ipc_errors(1);

        // Destroy the packet appropriately
        packetchain->DestroyPacket(packet);
        // Always delete the datachunk, we don't insert it into the packet
        // until later
        delete(datachunk);

        return NULL;
    }

    packet->insert(pack_comp_linkframe, datachunk);

    return packet;

}

bool KisDataSource::spawn_ipc() {
    stringstream ss;

    // Do not lock thread, we can only be called when we're inside a locked
    // context.
    // local_locker lock(&source_lock);
    
    set_source_running(false);
    set_child_pid(0);

    if (get_source_ipc_bin() == "") {
        ss << "Datasource '" << get_source_name() << "' missing IPC binary, cannot "
            "launch binary";
        _MSG(ss.str(), MSGFLAG_ERROR);

        // Call the handler if we have one
        if (error_callback != NULL)
            (*error_callback)(shared_ptr<KisDataSource>(this), error_aux);

        return false;
    }

    // Deregister from the handler if we have one
    if (ipchandler != NULL) {
        ipchandler->RemoveReadBufferInterface();
    }

    // Kill the running process if we have one
    if (source_ipc != NULL) {
        ss.str("");
        ss << "Datasource '" << get_source_name() << "' launching IPC with a running "
            "process, killing existing process pid " << get_child_pid();
        _MSG(ss.str(), MSGFLAG_INFO);

        source_ipc->soft_kill();
    }

    // Make a new handler and new ipc.  Give a generous buffer.
    ipchandler = new RingbufferHandler((32 * 1024), (32 * 1024));
    ipchandler->SetReadBufferInterface(this);

    source_ipc = new IPCRemoteV2(globalreg, ipchandler);

    // Get allowed paths for binaries
    vector<string> bin_paths = globalreg->kismet_config->FetchOptVec("bin_paths");

    for (vector<string>::iterator i = bin_paths.begin(); i != bin_paths.end(); ++i) {
        source_ipc->add_path(*i);
    }

    vector<string> args;

    int ret = source_ipc->launch_kis_binary(get_source_ipc_bin(), args);

    if (ret < 0) {
        ss.str("");
        ss << "Datasource '" << get_source_name() << "' failed to launch IPC " <<
            "binary '" << get_source_ipc_bin() << "'";
        _MSG(ss.str(), MSGFLAG_ERROR);

        // Call the handler if we have one
        if (error_callback != NULL)
            (*error_callback)(shared_ptr<KisDataSource>(this), error_aux);

        return false;
    }

    set_source_running(true);
    set_child_pid(source_ipc->get_pid());

    return true;
}

KisDataSource_QueuedCommand::KisDataSource_QueuedCommand(string in_cmd,
        KisDataSource::KVmap *in_kv, time_t in_time) {
    command = in_cmd;
    kv = in_kv;
    insert_time = in_time;
}

KisDataSource_CapKeyedObject::KisDataSource_CapKeyedObject(simple_cap_proto_kv *in_kp) {
    char ckey[17];

    snprintf(ckey, 17, "%s", in_kp->header.key);
    key = string(ckey);

    size = kis_ntoh32(in_kp->header.obj_sz);
    object = new char[size];
    memcpy(object, in_kp->object, size);
}

KisDataSource_CapKeyedObject::KisDataSource_CapKeyedObject(string in_key,
        const char *in_object, ssize_t in_len) {

    key = in_key.substr(0, 16);
    object = new char[in_len];
    memcpy(object, in_object, in_len);
}

KisDataSource_CapKeyedObject::~KisDataSource_CapKeyedObject() {
    delete[] object;
}

