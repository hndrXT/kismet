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

#ifndef __PACKETRACKER_H__
#define __PACKETRACKER_H__

#include "config.h"

#include <stdio.h>
#include <time.h>
#include <list>
#include <map>
#include <vector>
#include <algorithm>
#include <string>

#include "gpsd.h"
#include "packet.h"
#include "tracktypes.h"
#include "manuf.h"

// Extern over to kismet_server.cc to get our first time
extern time_t start_time;
// Extern over to kismet_server.cc to get our metric settings
extern unsigned int metric;

// Return values from ProcessPacket
#define TRACKER_NONE      0
#define TRACKER_NEW       1
#define TRACKER_NOTICE    2
#define TRACKER_ASSOCIATE 3
#define TRACKER_ALERT     4

class Packetracker {
public:
    Packetracker();
    ~Packetracker();

    // Do regular maintenance
    int Tick();

    // Get the error
    char *FetchError() { return errstr; }

    // Set up the gps
    void AddGPS(GPSD *in_gps);

    // Set up filtering - removed.  we do this in the server now before processing
    // anything else.
    //    void AddFilter(string in_filter) { filter = in_filter; }

    // Packet tracker stuff
    int ProcessPacket(packet_info info, char *in_status);
    int ProcessDataPacket(packet_info info, wireless_network *net, char *in_status);

    void UpdateIpdata(wireless_network *net);

    // Get all the networks
    vector<wireless_network *> FetchNetworks();

    int WriteNetworks(string in_fname);
    int WriteCSVNetworks(string in_fname);
    int WriteXMLNetworks(string in_fname);
    int WriteCisco(string in_fname);

    int WriteGpsdriveWaypt(FILE *in_file);

    void WriteSSIDMap(FILE *in_file);
    void ReadSSIDMap(FILE *in_file);

    void WriteIPMap(FILE *in_file);
    void ReadIPMap(FILE *in_file);

    // Tell the tracker to load maps of the manufacturer info
    void ReadAPManufMap(FILE *in_file);
    void ReadClientManufMap(FILE *in_file);

    // Convert the MAC
    static string Mac2String(uint8_t *mac, char seperator);
    // Utility to find strings that are empty or contain all spaces
    static bool IsBlank(const char *s);

    int FetchNumNetworks() { return num_networks; }
    //int FetchNumNetworks() { return bssid_map.size(); }
    int FetchNumPackets() { return num_packets; }
    int FetchNumDropped() { return num_dropped; }
    int FetchNumNoise() { return num_noise; }
    int FetchNumCrypt() { return num_crypt; }
    int FetchNumInteresting() { return num_interesting; }
    int FetchNumCisco() { return num_cisco; }

    void RemoveNetwork(mac_addr in_bssid);

protected:
    string SanitizeCSV(string in_data);
    string SanitizeXML(string in_data);

    char errstr[1024];
    GPSD *gps;

    int num_networks, num_packets, num_dropped, num_noise,
        num_crypt, num_interesting, num_cisco;

    // List of MAC's to filter
    //string filter;

    // all the networks
    vector<wireless_network *> network_list;

    // Several maps to refer to networks
    map<mac_addr, wireless_network *> bssid_map;

    // Map BSSID's to SSID for storage and cloaking
    map<mac_addr, string> bssid_cloak_map;

    // Map BSSID's to IP ranges for storage
    map<mac_addr, net_ip_data> bssid_ip_map;

    // Map probe responses for clients so we know we need to merge
    map<mac_addr, mac_addr> probe_map;

    // Manufacturer maps
    map<mac_addr, manuf *> ap_manuf_map;
    map<mac_addr, manuf *> client_manuf_map;

};

#endif

