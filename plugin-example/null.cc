
// include kismet headers
#include <config.h>

#include <stdio.h>
#include <string.h>

#include <globalregistry.h>
#include <plugintracker.h>
#include <timetracker.h>
#include <kis_netframe.h>

// Globals within the plugin
// Network protocol ref
int nullproto_ref;

// prototypes
int null_register(GlobalRegistry *);
int null_unregister(GlobalRegistry *);

// This has to be an extern "c" decl for the symbols to be found
extern "C" {
	int kis_plugin_info(plugin_usrdata *data) {
		data->pl_name = "Null";
		data->pl_version = "1.0.0";
		data->pl_description = "I do basically nothing";
		data->plugin_register = null_register;
		data->plugin_unregister = null_unregister;

		return 1;
	}
}

// Our silly plugin protocol
enum NULLPROTO_fields {
	NULLPROTO_text
};

char *NULLPROTO_fields_text[] = {
	"text",
	NULL
};

int Protocol_NULLPROTO(PROTO_PARMS) {
	for (unsigned int x = 0; x < field_vec->size(); x++) {
		switch((NULLPROTO_fields) (*field_vec)[x]) {
			case NULLPROTO_text:
				out_string += "\001I am generated by a plugin\001 ";
				break;
			default:
				out_string += "\001Unknown field requested\001 ";
				return -1;
				break;
		}
	}

	return 1;
}

void Protocol_NULLPROTO_Enable(PROTO_ENABLE_PARMS) {
	kis_protocol_cache cache;
	globalreg->kisnetserver->SendToClient(in_fd, nullproto_ref, NULL, &cache);
}

int null_register(GlobalRegistry *in_globalreg) {
	printf("PLUGINDEBUG - Register got called\n");

	if (in_globalreg->kisnetserver == NULL) {
		printf("PLUGINDEBUG - Got called before netserver exists\n");
		return 0;
	}

	nullproto_ref = 
		in_globalreg->kisnetserver->RegisterProtocol("NULLFOO", 0, 0,
													 NULLPROTO_fields_text,
													 &Protocol_NULLPROTO,
													 &Protocol_NULLPROTO_Enable);

	return 1;
}

int null_unregister(GlobalRegistry *in_globalreg) {
	printf("PLUGINDEBUG - I got unregistered\n");

	return 1;
}

