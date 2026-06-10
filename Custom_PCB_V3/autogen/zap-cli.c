/***************************************************************************//**
 * @file  zap-cli.c
 * @brief Generated file for zcl cli using ZAP. Do not update file manually.
 *******************************************************************************
 * # License
 * <b>Copyright 2020 Silicon Laboratories Inc. www.silabs.com</b>
 *******************************************************************************
 *
 * The licensor of this software is Silicon Laboratories Inc. Your use of this
 * software is governed by the terms of Silicon Labs Master Software License
 * Agreement (MSLA) available at
 * www.silabs.com/about-us/legal/master-software-license-agreement. This
 * software is distributed to you in Source Code format and is governed by the
 * sections of the MSLA applicable to Source Code.
 *
 ******************************************************************************/

#ifdef SL_COMPONENT_CATALOG_PRESENT
#include "sl_component_catalog.h"
#endif

#ifdef SL_CATALOG_ZIGBEE_ZCL_CLI_PRESENT

#include <stdlib.h>
#include "sl_cli_config.h"
#include "sl_cli_command.h"
#include "sl_cli.h"
#include "zcl-cli.h"
#ifdef SL_CATALOG_ZIGBEE_DEBUG_PRINT_PRESENT
#include "sl_zigbee_debug_print.h"
#endif // SL_CATALOG_ZIGBEE_DEBUG_PRINT_PRESENT
#include "zap-type.h"
#include "zap-id.h"
#include "app/framework/include/af.h"
#include "zap-config.h"

extern void sli_zigbee_zcl_simple_command(uint8_t frameControl,
                        uint16_t clusterId,
                        uint8_t commandId,
                        sl_cli_command_arg_t *arguments,
                        uint8_t *argumentTypes);
extern sl_cli_command_info_t cli_cmd_zcl_global_group;
extern sl_cli_command_info_t cli_cmd_zcl_mfg_code_command;
extern sl_cli_command_info_t cli_cmd_zcl_time_command;
extern sl_cli_command_info_t cli_cmd_zcl_use_next_sequence_command;
extern sl_cli_command_info_t cli_cmd_zcl_x_default_resp_command;
extern sl_cli_command_info_t cli_cmd_zcl_test_response_on_command;
extern sl_cli_command_info_t cli_cmd_zcl_test_response_off_command;
#if defined(ZCL_USING_IDENTIFY_CLUSTER_SERVER) || defined(ZCL_USING_IDENTIFY_CLUSTER_CLIENT)
extern sl_cli_command_info_t cli_cmd_zcl_identify_on_command;
extern sl_cli_command_info_t cli_cmd_zcl_identify_off_command;
#endif
#if defined(ZCL_USING_BACNET_PROTOCOL_TUNNEL_CLUSTER_CLIENT) || defined(ZCL_USING_BACNET_PROTOCOL_TUNNEL_CLUSTER_SERVER)
extern sl_cli_command_info_t cli_cmd_zcl_bacnet_transfer_whois_command;
#endif
#if defined(ZCL_USING_TUNNELING_CLUSTER_CLIENT) || defined(ZCL_USING_TUNNELING_CLUSTER_SERVER)
extern sl_cli_command_info_t cli_cmd_zcl_tunneling_random_to_server_command;
extern sl_cli_command_info_t cli_cmd_zcl_tunneling_transfer_to_server_command;
extern sl_cli_command_info_t cli_cmd_zcl_tunneling_random_to_client_command;
extern sl_cli_command_info_t cli_cmd_zcl_tunneling_transfer_to_client_command;
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Provide function declarations
void sli_zigbee_cli_zcl_identify_ez_mode_command(sl_cli_command_arg_t *arguments);
void sli_zigbee_cli_zcl_identify_id_command(sl_cli_command_arg_t *arguments);
void sli_zigbee_cli_zcl_identify_query_command(sl_cli_command_arg_t *arguments);
void sli_zigbee_cli_zcl_basic_rtfd_command(sl_cli_command_arg_t *arguments);
void sli_zigbee_cli_zcl_identify_trigger_command(sl_cli_command_arg_t *arguments);

// Command structs. Names are command names prefixed by cli_cmd_zcl_[cluster name]_cluster
static const sl_cli_command_info_t cli_cmd_zcl_identify_cluster_ez_mode_invoke = \
SL_CLI_COMMAND(sli_zigbee_cli_zcl_identify_ez_mode_command,
            "Invoke EZMode on an Identify Server",
            "action" SL_CLI_UNIT_SEPARATOR ,
            {
                SL_CLI_ARG_UINT8,
                SL_CLI_ARG_END,
            });

static const sl_cli_command_info_t cli_cmd_zcl_identify_cluster_identify = \
SL_CLI_COMMAND(sli_zigbee_cli_zcl_identify_id_command,
            "Command description for Identify",
            "identify time" SL_CLI_UNIT_SEPARATOR ,
            {
                SL_CLI_ARG_UINT16,
                SL_CLI_ARG_END,
            });

static const sl_cli_command_info_t cli_cmd_zcl_identify_cluster_identify_query = \
SL_CLI_COMMAND(sli_zigbee_cli_zcl_identify_query_command,
            "Command description for IdentifyQuery",
            "",
            {
                SL_CLI_ARG_END,
            });

static const sl_cli_command_info_t cli_cmd_zcl_basic_cluster_reset_to_factory_defaults = \
SL_CLI_COMMAND(sli_zigbee_cli_zcl_basic_rtfd_command,
            "Command that resets all attribute values to factory default.",
            "",
            {
                SL_CLI_ARG_END,
            });

static const sl_cli_command_info_t cli_cmd_zcl_identify_cluster_trigger_effect = \
SL_CLI_COMMAND(sli_zigbee_cli_zcl_identify_trigger_command,
            "Command description for TriggerEffect",
            "effect id" SL_CLI_UNIT_SEPARATOR "effect variant" SL_CLI_UNIT_SEPARATOR ,
            {
                SL_CLI_ARG_UINT8,
                SL_CLI_ARG_UINT8,
                SL_CLI_ARG_END,
            });


// Create group command tables and structs if cli_groups given
// in template. Group name is suffixed with [cluster name]_[cluster_side]_cluster_group_table for tables
// and group commands are cli_cmd_( group name )_group
static const sl_cli_command_entry_t zcl_basic_cluster_command_table[] = {
    { "rtfd", &cli_cmd_zcl_basic_cluster_reset_to_factory_defaults, false },
    { NULL, NULL, false },
};
static const sl_cli_command_entry_t zcl_identify_cluster_command_table[] = {
    { "id", &cli_cmd_zcl_identify_cluster_identify, false },
    { "query", &cli_cmd_zcl_identify_cluster_identify_query, false },
    { "ez-mode", &cli_cmd_zcl_identify_cluster_ez_mode_invoke, false },
    { "trigger", &cli_cmd_zcl_identify_cluster_trigger_effect, false },
    { "on", &cli_cmd_zcl_identify_on_command, false},
    { "off", &cli_cmd_zcl_identify_off_command, false},
    { NULL, NULL, false },
};

// ZCL cluster commands
static const sl_cli_command_info_t cli_cmd_basic_group = \
  SL_CLI_COMMAND_GROUP(zcl_basic_cluster_command_table, "ZCL basic cluster commands");
static const sl_cli_command_info_t cli_cmd_identify_group = \
  SL_CLI_COMMAND_GROUP(zcl_identify_cluster_command_table, "ZCL identify cluster commands");

static const sl_cli_command_entry_t zcl_group_table[] = {
  { "basic", &cli_cmd_basic_group, false },
  { "identify", &cli_cmd_identify_group, false },
  { "global", &cli_cmd_zcl_global_group, false },
  { "mfg-code", &cli_cmd_zcl_mfg_code_command, false},
  { "time", &cli_cmd_zcl_time_command, false},
  { "use-next-sequence", &cli_cmd_zcl_use_next_sequence_command, false},
  { "x-default-resp", &cli_cmd_zcl_x_default_resp_command, false},
  { "test-response-on", &cli_cmd_zcl_test_response_on_command, false},
  { "test-response-off", &cli_cmd_zcl_test_response_off_command, false},
  { NULL, NULL, false },
};

sl_cli_command_info_t cli_cmd_zcl_group = \
  SL_CLI_COMMAND_GROUP(zcl_group_table, "ZCL commands");

// Create root command table
const sl_cli_command_entry_t sl_cli_zcl_command_table[] = {
  { "zcl", &cli_cmd_zcl_group, false },
  { NULL, NULL, false },
};

sl_cli_command_group_t sl_cli_zcl_command_group =
{
  { NULL },
  false,
  sl_cli_zcl_command_table
};


WEAK(void sli_zigbee_cli_zcl_identify_ez_mode_command(sl_cli_command_arg_t *arguments)) {
  uint8_t argumentTypes[1] =  { 
    SL_ZCL_CLI_ARG_UINT8
  }; 
  sli_zigbee_zcl_simple_command(
    ZCL_CLUSTER_SPECIFIC_COMMAND | ZCL_FRAME_CONTROL_CLIENT_TO_SERVER,  \
    ZCL_IDENTIFY_CLUSTER_ID,                                                       \
    2, \
    arguments, \
    argumentTypes);
}

WEAK(void sli_zigbee_cli_zcl_identify_id_command(sl_cli_command_arg_t *arguments)) {
  uint8_t argumentTypes[1] =  { 
    SL_ZCL_CLI_ARG_UINT16
  }; 
  sli_zigbee_zcl_simple_command(
    ZCL_CLUSTER_SPECIFIC_COMMAND | ZCL_FRAME_CONTROL_CLIENT_TO_SERVER,  \
    ZCL_IDENTIFY_CLUSTER_ID,                                                       \
    0, \
    arguments, \
    argumentTypes);
}

WEAK(void sli_zigbee_cli_zcl_identify_query_command(sl_cli_command_arg_t *arguments)) {
  sli_zigbee_zcl_simple_command(
    ZCL_CLUSTER_SPECIFIC_COMMAND | ZCL_FRAME_CONTROL_CLIENT_TO_SERVER,  \
    ZCL_IDENTIFY_CLUSTER_ID,                                                       \
    1, \
    arguments, \
    NULL);
}

WEAK(void sli_zigbee_cli_zcl_basic_rtfd_command(sl_cli_command_arg_t *arguments)) {
  sli_zigbee_zcl_simple_command(
    ZCL_CLUSTER_SPECIFIC_COMMAND | ZCL_FRAME_CONTROL_CLIENT_TO_SERVER,  \
    ZCL_BASIC_CLUSTER_ID,                                                       \
    0, \
    arguments, \
    NULL);
}

WEAK(void sli_zigbee_cli_zcl_identify_trigger_command(sl_cli_command_arg_t *arguments)) {
  uint8_t argumentTypes[2] =  { 
    SL_ZCL_CLI_ARG_UINT8,
    SL_ZCL_CLI_ARG_UINT8
  }; 
  sli_zigbee_zcl_simple_command(
    ZCL_CLUSTER_SPECIFIC_COMMAND | ZCL_FRAME_CONTROL_CLIENT_TO_SERVER,  \
    ZCL_IDENTIFY_CLUSTER_ID,                                                       \
    64, \
    arguments, \
    argumentTypes);
}

#ifdef __cplusplus
}
#endif
#endif