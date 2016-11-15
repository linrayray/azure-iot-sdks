// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include <stdlib.h>
#ifdef _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif
#include <limits.h>
#include "iothubtransport_amqp_connection.h"
#include "azure_c_shared_utility/agenttime.h"
#include "azure_c_shared_utility/strings.h"
#include "azure_c_shared_utility/uniqueid.h"
#include "azure_uamqp_c/sasl_mechanism.h"
#include "azure_uamqp_c/saslclientio.h"
#include "azure_uamqp_c/sasl_mssbcbs.h"
#include "azure_uamqp_c/connection.h"

#define RESULT_OK 0
#define INDEFINITE_TIME ((time_t)(-1))
#define DEFAULT_INCOMING_WINDOW_SIZE UINT_MAX
#define DEFAULT_OUTGOING_WINDOW_SIZE 100
#define SASL_IO_OPTION_LOG_TRACE "logtrace"

typedef struct AMQP_CONNECTION_INSTANCE_TAG
{
	STRING_HANDLE iothub_fqdn;
	XIO_HANDLE underlying_io_transport;
	CBS_HANDLE cbs_handle;
	CONNECTION_HANDLE connection_handle;
	SESSION_HANDLE session_handle;
	XIO_HANDLE sasl_io;
	SASL_MECHANISM_HANDLE sasl_mechanism;
	bool is_trace_on;
	AMQP_CONNECTION_STATE current_state;
	ON_AMQP_CONNECTION_STATE_CHANGED on_state_changed_callback;
	const void* on_state_changed_context;
	ON_CONNECTION_ERROR_CALLBACK on_error_callback;
	const void* on_error_context;
} AMQP_CONNECTION_INSTANCE;


static void destroy_sasl_components(AMQP_CONNECTION_INSTANCE* instance)
{
	if (instance->sasl_io != NULL)
	{
		xio_destroy(instance->sasl_io);
		instance->sasl_io = NULL;
	}

	if (instance->sasl_mechanism != NULL)
	{
		saslmechanism_destroy(instance->sasl_mechanism);
		instance->sasl_mechanism = NULL;
	}
}

static XIO_HANDLE create_sasl_components(AMQP_CONNECTION_INSTANCE* instance)
{
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_012: [`instance->sasl_mechanism` shall be created using saslmechanism_create()]
	if ((instance->sasl_mechanism = saslmechanism_create(saslmssbcbs_get_interface(), NULL)) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_013: [If saslmechanism_create() fails, amqp_connection_create() shall fail and return NULL]
		LogError("Failed creating the SASL mechanism (saslmechanism_create failed)");
	}
	else
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_014: [A SASLCLIENTIO_CONFIG shall be set with `instance->underlying_io_transport` and `instance->sasl_mechanism`]
		SASLCLIENTIO_CONFIG sasl_client_config;
		sasl_client_config.sasl_mechanism = instance->sasl_mechanism;
		sasl_client_config.underlying_io = instance->underlying_io_transport;
		
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_015: [`instance->sasl_io` shall be created using xio_create() passing saslclientio_get_interface_description() and the SASLCLIENTIO_CONFIG instance]
		if ((instance->sasl_io = xio_create(saslclientio_get_interface_description(), &sasl_client_config)) == NULL)
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_016: [If xio_create() fails, amqp_connection_create() shall fail and return NULL]
			LogError("Failed creating the SASL I/O (xio_create failed)");
			destroy_sasl_components(instance);
		}
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_017: [The sasl_io "logtrace" option shall be set using xio_setoption(), passing `instance->is_trace_on`]
		else if (xio_setoption(instance->sasl_io, SASL_IO_OPTION_LOG_TRACE, (const void*)&instance->is_trace_on) != RESULT_OK)
		{
			LogError("Failed setting the SASL I/O logging trace option (xio_setoption failed)");
			destroy_sasl_components(instance);
		}
	}

	return instance->sasl_io;
}

static void on_connection_io_error(void* context)
{
	(void)context;
	// TODO: implement this.
	// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_022: [If the connection calls back with an I/O error, `instance->on_error_callback` shall be invoked if set passing code CONNECTION_ERROR and `instance->on_error_context`]
}

static void update_state(AMQP_CONNECTION_INSTANCE* instance, AMQP_CONNECTION_STATE new_state)
{
	if (new_state != instance->current_state)
	{
		AMQP_CONNECTION_STATE previous_state = instance->current_state;
		instance->current_state = new_state;

		if (instance->on_state_changed_callback != NULL)
		{
			instance->on_state_changed_callback(instance->on_state_changed_context, previous_state, new_state);
		}
	}
}

static void on_connection_state_changed(const void* context, CONNECTION_STATE new_connection_state, CONNECTION_STATE previous_connection_state)
{
	AMQP_CONNECTION_INSTANCE* instance = (AMQP_CONNECTION_INSTANCE*)context;

	// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_062: [If `on_connection_state_changed` is called back but CBS connection has been created, it shall return immediatelly]
	if (instance->cbs_handle == NULL && new_connection_state != previous_connection_state)
	{
		// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_063: [If `on_connection_state_changed` is called back, `instance->on_state_changed_callback` shall be invoked, if defined, only if the new state is different than the previous]

		// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_064: [If `on_connection_state_changed` new state is CONNECTION_STATE_OPENED, `instance->on_state_changed_callback` shall be invoked with state AMQP_CONNECTION_STATE_OPENED]
		if (new_connection_state == CONNECTION_STATE_OPENED)
		{
			update_state(instance, AMQP_CONNECTION_STATE_OPENED);
		}
		// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_065: [If `on_connection_state_changed` new state is CONNECTION_STATE_END or CONNECTION_STATE_ERROR, `instance->on_state_changed_callback` shall be invoked with state AMQP_CONNECTION_STATE_CLOSED]
		else if (new_connection_state == CONNECTION_STATE_END)
		{
			update_state(instance, AMQP_CONNECTION_STATE_CLOSED);
		}
	}
}

static int create_connection_handle(AMQP_CONNECTION_INSTANCE* instance)
{
	int result;
	char* unique_container_id = NULL;
	XIO_HANDLE connection_io_transport;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_007: [If `instance->sasl_io` is defined it shall be used as parameter `xio` in connection_create2()]
	if (instance->sasl_io != NULL)
	{
		connection_io_transport = instance->sasl_io;
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_018: [If `instance->sasl_io` is not defined, `instance->underlying_io_transport` shall be used as parameter `xio` in connection_create2()]
	else
	{
		connection_io_transport = instance->underlying_io_transport;
	}

	if (UniqueId_Generate(unique_container_id, 16) != UNIQUEID_OK)
	{
		result = __LINE__;
		LogError("failed creating the AMQP connection (UniqueId_Generate failed)");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_019: [`instance->connection_handle` shall be created using connection_create2(), passing the `connection_underlying_io`, `instance->iothub_host_fqdn` and an unique string as container ID]
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_020: [connection_create2() shall also receive `on_connection_state_changed` and `on_connection_error` callback functions]
	else if ((instance->connection_handle = connection_create2(connection_io_transport, STRING_c_str(instance->iothub_fqdn), unique_container_id, NULL, NULL, on_connection_state_changed, (void*)instance, on_connection_io_error, (void*)instance)) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_021: [If connection_create2() fails, amqp_connection_create() shall fail and return NULL]
		result = __LINE__;
		LogError("failed creating the AMQP connection (connection_create2 failed)");
	}
	else
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_023: [The connection tracing shall be set using connection_set_trace(), passing `instance->is_trace_on`]
		connection_set_trace(instance->connection_handle, instance->is_trace_on);

		result = RESULT_OK;
	}

	if (unique_container_id != NULL)
	{
		free(unique_container_id);
	}

	return result;
}

static int create_session_handle(AMQP_CONNECTION_INSTANCE* instance)
{
	int result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_024: [`instance->session_handle` shall be created using session_create(), passing `instance->connection_handle`]
	if ((instance->session_handle = session_create(instance->connection_handle, NULL, NULL)) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_025: [If session_create() fails, amqp_connection_create() shall fail and return NULL]
		result = __LINE__;
		LogError("Failed creating the AMQP connection (connection_create2 failed)");
	}
	else
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_026: [The `instance->session_handle` incoming window size shall be set as UINT_MAX using session_set_incoming_window()]
		if (session_set_incoming_window(instance->session_handle, (uint32_t)DEFAULT_INCOMING_WINDOW_SIZE) != 0)
		{
			LogError("Failed to set the AMQP session incoming window size.");
		}

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_027: [The `instance->session_handle` outgoing window size shall be set as 100 using session_set_outgoing_window()]
		if (session_set_outgoing_window(instance->session_handle, DEFAULT_OUTGOING_WINDOW_SIZE) != 0)
		{
			LogError("Failed to set the AMQP session outgoing window size.");
		}

		result = RESULT_OK;
	}
	
	return result;
}

static void on_cbs_state_changed(void* context, AMQP_MANAGEMENT_STATE new_amqp_management_state, AMQP_MANAGEMENT_STATE previous_amqp_management_state)
{
	if (new_amqp_management_state != previous_amqp_management_state)
	{
		AMQP_CONNECTION_INSTANCE* instance = (AMQP_CONNECTION_INSTANCE*)context;

		// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_063: [If `on_cbs_state_changed` is called back, `instance->on_state_changed_callback` shall be invoked, if defined, only if the new state is different than the previous]

		// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_064: [If `on_cbs_state_changed` new state is AMQP_MANAGEMENT_STATE_OPEN, `instance->on_state_changed_callback` shall be invoked with state AMQP_CONNECTION_STATE_OPENED]
		if (new_amqp_management_state == AMQP_MANAGEMENT_STATE_OPEN)
		{
			update_state(instance, AMQP_CONNECTION_STATE_OPENED);
		}
		// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_065: [If `on_cbs_state_changed` new state is AMQP_MANAGEMENT_STATE_IDLE or AMQP_MANAGEMENT_STATE_ERROR, `instance->on_state_changed_callback` shall be invoked with state AMQP_CONNECTION_STATE_CLOSED]
		else if (new_amqp_management_state == AMQP_MANAGEMENT_STATE_IDLE)
		{
			update_state(instance, AMQP_CONNECTION_STATE_CLOSED);
		}
	}
}

static int create_cbs_handle(AMQP_CONNECTION_INSTANCE* instance)
{
	int result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_029: [`instance->cbs_handle` shall be created using cbs_create(), passing `instance->session_handle` and `on_cbs_state_changed` callback]
	if ((instance->cbs_handle = cbs_create(instance->session_handle, on_cbs_state_changed, (void*)instance)) == NULL)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_030: [If cbs_create() fails, amqp_connection_create() shall fail and return NULL]
		result = __LINE__;
		LogError("Failed to create the CBS connection.");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_031: [`instance->cbs_handle` shall be opened using cbs_open()]
	else if (cbs_open(instance->cbs_handle) != RESULT_OK)
	{
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_032: [If cbs_open() fails, amqp_connection_create() shall fail and return NULL]
		result = __LINE__;
		LogError("Failed to open the connection with CBS.");
	}
	else
	{
		result = RESULT_OK;
	}

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_033: [If the cbs_open() calls back with an error, `instance->on_error_callback` shall be invoked if set passing code CONNECTION_CBS_ERROR and `instance->on_error_context`]
	return result;
}


// Public APIS:

AMQP_CONNECTION_HANDLE amqp_connection_create(AMQP_CONNECTION_CONFIG* config)
{
	AMQP_CONNECTION_HANDLE result;

	// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_001: [If `config` is NULL, amqp_connection_create() shall fail and return NULL]
	if (config == NULL)
	{
		result = NULL;
		LogError("amqp_connection_create failed (config is NULL)");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_002: [If `config->iothub_host_fqdn` is NULL, amqp_connection_create() shall fail and return NULL]
	else if (config->iothub_host_fqdn == NULL)
	{
		result = NULL;
		LogError("amqp_connection_create failed (config->iothub_host_fqdn is NULL)");
	}
	// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_003: [If `config->underlying_io_transport` is NULL, amqp_connection_create() shall fail and return NULL]
	else if (config->underlying_io_transport == NULL)
	{
		result = NULL;
		LogError("amqp_connection_create failed (config->underlying_io_transport is NULL)");
	}
	else
	{
		AMQP_CONNECTION_INSTANCE* instance;

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_057: [amqp_connection_create() shall allocate memory for an instance of the connection state]
		if ((instance = (AMQP_CONNECTION_INSTANCE*)malloc(sizeof(AMQP_CONNECTION_INSTANCE))) == NULL)
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_058: [If malloc() fails, amqp_connection_create() shall fail and return NULL]
			result = NULL;
			LogError("amqp_connection_create failed (malloc failed)");
		}
		// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_005: [A copy of `config->iothub_host_fqdn` shall be saved on `instance->iothub_host_fqdn`]
		else if ((instance->iothub_fqdn = STRING_construct(config->iothub_host_fqdn)) == NULL)
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_066: [If STRING_construct() fails, amqp_connection_create() shall fail and return NULL]
			result = NULL;
			LogError("amqp_connection_create failed (STRING_construct failed)");
		}
		else
		{
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_006: [`config->underlying_io_transport` shall be saved on `instance->underlying_io_transport`]
			instance->underlying_io_transport = config->underlying_io_transport;
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_008: [`config->is_trace_on` shall be saved on `instance->is_trace_on`]
			instance->is_trace_on = config->is_trace_on;
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_060: [`config->on_state_changed_callback` shall be saved on `instance->on_state_changed_callback`]
			instance->on_state_changed_callback = config->on_state_changed_callback;
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_061: [`config->on_state_changed_context` shall be saved on `instance->on_state_changed_context`]
			instance->on_state_changed_context = config->on_state_changed_context;
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_009: [`config->on_error_callback` shall be saved on `instance->on_error_callback`]
			instance->on_error_callback = config->on_error_callback;
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_010: [`config->on_error_context` shall be saved on `instance->on_error_context`]
			instance->on_error_context = config->on_error_context;
			
			instance->sasl_io = NULL;
			instance->sasl_mechanism = NULL;
			instance->connection_handle = NULL;
			instance->session_handle = NULL;

			// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_011: [If `config->create_sasl_io` is true or `config->create_cbs_connection` is true, amqp_connection_create() shall create SASL I/O]
			if ((config->create_sasl_io || config->create_cbs_connection) && create_sasl_components(instance) != RESULT_OK)
			{
				result = NULL;
				LogError("amqp_connection_create failed (failed creating the SASL components)");
			}
			else if (create_connection_handle(instance) != RESULT_OK)
			{
				result = NULL;
				LogError("amqp_connection_create failed (failed creating the AMQP connection)");
			}
			else if (create_session_handle(instance) != RESULT_OK)
			{
				result = NULL;
				LogError("amqp_connection_create failed (failed creating the AMQP session)");
			}
			// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_028: [Only if `config->create_cbs_connection` is true, amqp_connection_create() shall create and open the CBS_HANDLE]
			else if (config->create_cbs_connection && create_cbs_handle(instance) != RESULT_OK)
			{
				result = NULL;
				LogError("amqp_connection_create failed (failed creating the CBS handle)");
			}
			else
			{
				// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_034: [If no failures occur, amqp_connection_create() shall return the handle to the connection state]
				result = (AMQP_CONNECTION_HANDLE)instance;
			}
		}
	}

	return result;
}

void amqp_connection_destroy(AMQP_CONNECTION_HANDLE conn_handle)
{
	// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_035: [If `conn_handle` is NULL, amqp_connection_destroy() shall fail and return]
	if (conn_handle != NULL)
	{
		AMQP_CONNECTION_INSTANCE* instance = (AMQP_CONNECTION_INSTANCE*)conn_handle;

		// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_036: [amqp_connection_destroy() shall destroy `instance->cbs_handle` if set using cbs_destroy()]
		if (instance->cbs_handle != NULL)
		{
			cbs_destroy(instance->cbs_handle);
		}

		// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_037: [amqp_connection_destroy() shall destroy `instance->session_handle` if set using session_destroy()]
		if (instance->session_handle != NULL)
		{
			session_destroy(instance->session_handle);
		}

		// Codes_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_067: [amqp_connection_destroy() shall destroy `instance->connection_handle` if set using connection_destroy()]
		if (instance->connection_handle != NULL)
		{
			connection_destroy(instance->connection_handle);
		}

		// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_038: [amqp_connection_destroy() shall destroy `instance->sasl_io` if set using xio_destroy()]
		if (instance->sasl_io != NULL)
		{
			xio_destroy(instance->sasl_io);
		}
		
		// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_039: [amqp_connection_destroy() shall destroy `instance->sasl_mechanism` if set using saslmechanism_destroy()]
		if (instance->sasl_mechanism != NULL)
		{
			saslmechanism_destroy(instance->sasl_mechanism);
		}

		// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_059: [amqp_connection_destroy() shall destroy `instance->iothub_host_fqdn` if set using STRING_delete()]
		if (instance->iothub_fqdn != NULL)
		{
			STRING_delete(instance->iothub_fqdn);
		}

		// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_040: [amqp_connection_destroy() shall free the memory allocated for the connection instance]
		free(instance);
	}
}

void amqp_connection_do_work(AMQP_CONNECTION_HANDLE conn_handle)
{
	// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_041: [If `conn_handle` is NULL, amqp_connection_do_work() shall fail and return]
	if (conn_handle != NULL)
	{
		AMQP_CONNECTION_INSTANCE* instance = (AMQP_CONNECTION_INSTANCE*)conn_handle;

		// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_042: [connection_dowork() shall be invoked passing `instance->connection_handle`]
		connection_dowork(instance->connection_handle);
	}
}

int amqp_connection_get_session_handle(AMQP_CONNECTION_HANDLE conn_handle, SESSION_HANDLE* session_handle)
{
	int result;
	
	// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_043: [If `conn_handle` is NULL, amqp_connection_get_session_handle() shall fail and return __LINE__]
	if (conn_handle == NULL)
	{
		result = __LINE__;
		LogError("amqp_connection_get_session_handle failed (conn_handle is NULL)");
	}
	// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_044: [If `session_handle` is NULL, amqp_connection_get_session_handle() shall fail and return __LINE__]
	else if (session_handle == NULL)
	{
		result = __LINE__;
		LogError("amqp_connection_get_session_handle failed (session_handle is NULL)");
	}
	else
	{
		AMQP_CONNECTION_INSTANCE* instance = (AMQP_CONNECTION_INSTANCE*)conn_handle;

		// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_045: [`session_handle` shall be set to point to `instance->session_handle`]
		*session_handle = instance->session_handle;

		// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_046: [amqp_connection_get_session_handle() shall return success code 0]
		result = RESULT_OK;
	}

	return result;
}

int amqp_connection_get_cbs_handle(AMQP_CONNECTION_HANDLE conn_handle, CBS_HANDLE* cbs_handle)
{
	int result;

	// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_047: [If `conn_handle` is NULL, amqp_connection_get_cbs_handle() shall fail and return __LINE__]
	if (conn_handle == NULL)
	{
		result = __LINE__;
		LogError("amqp_connection_get_cbs_handle failed (conn_handle is NULL)");
	}
	// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_048: [If `cbs_handle` is NULL, amqp_connection_get_cbs_handle() shall fail and return __LINE__]
	else if (cbs_handle == NULL)
	{
		result = __LINE__;
		LogError("amqp_connection_get_cbs_handle failed (parameter cbs_handle is NULL)");
	}
	else
	{
		AMQP_CONNECTION_INSTANCE* instance = (AMQP_CONNECTION_INSTANCE*)conn_handle;

		// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_049: [If `instance->cbs_handle` is NULL, amqp_connection_get_cbs_handle() shall fail and return __LINE__]
		if (instance->cbs_handle == NULL)
		{
			result = __LINE__;
			LogError("amqp_connection_get_cbs_handle failed (there is not a cbs_handle to be returned)");
		}
		else
		{
			// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_050: [`cbs_handle` shall be set to point to `instance->cbs_handle`]
			*cbs_handle = instance->cbs_handle;

			// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_051: [amqp_connection_get_cbs_handle() shall return success code 0]
			result = RESULT_OK;
		}
	}

	return result;
}

int amqp_connection_set_logging(AMQP_CONNECTION_HANDLE conn_handle, bool is_trace_on)
{
	int result;

	// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_052: [If `conn_handle` is NULL, amqp_connection_set_tracing() shall fail and return __LINE__]
	if (conn_handle == NULL)
	{
		result = __LINE__;
		LogError("amqp_connection_set_logging failed (conn_handle is NULL)");
	}
	else
	{
		AMQP_CONNECTION_INSTANCE* instance = (AMQP_CONNECTION_INSTANCE*)conn_handle;

		// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_053: [`instance->is_trace_on` shall be set to `is_trace_on`]
		instance->is_trace_on = is_trace_on;

		// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_054: [Tracing on `instance->sasl_io` shall be set to `instance->is_trace_on` if the value has changed]
		if (instance->sasl_io != NULL &&
			xio_setoption(instance->sasl_io, SASL_IO_OPTION_LOG_TRACE, (const void*)&instance->is_trace_on) != RESULT_OK)
		{
			result = __LINE__;
			LogError("amqp_connection_set_logging failed (xio_setoption() failed)");
		}
		else
		{
			// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_055: [Tracing on `instance->connection_handle` shall be set to `instance->is_trace_on` if the value has changed]
			connection_set_trace(instance->connection_handle, instance->is_trace_on);

			// Code_SRS_IOTHUBTRANSPORT_AMQP_CONNECTION_09_056: [amqp_connection_set_tracing() shall return success code 0]
			result = RESULT_OK;
		}
	}

	return result;
}