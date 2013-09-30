/**************************************************************************/
/*!
    @file     hub.c
    @author   hathach (tinyusb.org)

    @section LICENSE

    Software License Agreement (BSD License)

    Copyright (c) 2013, hathach (tinyusb.org)
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
    1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
    3. Neither the name of the copyright holders nor the
    names of its contributors may be used to endorse or promote products
    derived from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ''AS IS'' AND ANY
    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

    This file is part of the tinyusb stack.
*/
/**************************************************************************/

#include "tusb_option.h"

#if (MODE_HOST_SUPPORTED && TUSB_CFG_HOST_HUB)

#define _TINY_USB_SOURCE_FILE_

//--------------------------------------------------------------------+
// INCLUDE
//--------------------------------------------------------------------+
#include "hub.h"
#include "usbh_hcd.h"

//--------------------------------------------------------------------+
// MACRO CONSTANT TYPEDEF
//--------------------------------------------------------------------+
typedef struct {
  pipe_handle_t pipe_status;
  uint8_t interface_number;
  uint8_t port_number;
  uint8_t status_change; // data from status change interrupt endpoint
}usbh_hub_t;

usbh_hub_t hub_data[TUSB_CFG_HOST_DEVICE_MAX] TUSB_CFG_ATTR_USBRAM;
uint8_t hub_enum_buffer[sizeof(descriptor_hub_desc_t)] TUSB_CFG_ATTR_USBRAM;

//--------------------------------------------------------------------+
// HUB
//--------------------------------------------------------------------+
tusb_error_t hub_port_clear_feature_subtask(uint8_t feature)
{
  tusb_error_t error;

  OSAL_SUBTASK_BEGIN

  SUBTASK_ASSERT(HUB_FEATURE_PORT_CONNECTION_CHANGE <= feature &&
                 feature <= HUB_FEATURE_PORT_RESET_CHANGE);

  //------------- Clear Port Feature request -------------//
  OSAL_SUBTASK_INVOKED_AND_WAIT(
      usbh_control_xfer_subtask( usbh_devices[0].hub_addr, bm_request_type(TUSB_DIR_HOST_TO_DEV, TUSB_REQUEST_TYPE_CLASS, TUSB_REQUEST_RECIPIENT_OTHER),
                                 HUB_REQUEST_CLEAR_FEATURE, feature, usbh_devices[0].hub_port,
                                 0, NULL ),
      error
  );
  SUBTASK_ASSERT_STATUS( error );

  //------------- Get Port Status to check if feature is cleared -------------//
  OSAL_SUBTASK_INVOKED_AND_WAIT(
      usbh_control_xfer_subtask( usbh_devices[0].hub_addr, bm_request_type(TUSB_DIR_DEV_TO_HOST, TUSB_REQUEST_TYPE_CLASS, TUSB_REQUEST_RECIPIENT_OTHER),
                                 HUB_REQUEST_GET_STATUS, 0, usbh_devices[0].hub_port,
                                 4, hub_enum_buffer ),
      error
  );
  SUBTASK_ASSERT_STATUS( error );

  //------------- Check if feature is cleared -------------//
  hub_port_status_response_t * p_port_status = (hub_port_status_response_t *) hub_enum_buffer;
  SUBTASK_ASSERT( !BIT_TEST_(p_port_status->status_change.value, feature-16)  );

  OSAL_SUBTASK_END
}

tusb_error_t hub_enumerate_subtask(void)
{
  tusb_error_t error;

  OSAL_SUBTASK_BEGIN

  hub_port_status_response_t * p_port_status;

  //------------- Get Port Status -------------//
  OSAL_SUBTASK_INVOKED_AND_WAIT(
      usbh_control_xfer_subtask( usbh_devices[0].hub_addr, bm_request_type(TUSB_DIR_DEV_TO_HOST, TUSB_REQUEST_TYPE_CLASS, TUSB_REQUEST_RECIPIENT_OTHER),
                                 HUB_REQUEST_GET_STATUS, 0, usbh_devices[0].hub_port,
                                 4, hub_enum_buffer ),
      error
  );
  SUBTASK_ASSERT_STATUS( error );

  p_port_status = (hub_port_status_response_t *) hub_enum_buffer;
  if ( !p_port_status->status_change.connect_status )   SUBTASK_EXIT(TUSB_ERROR_NONE); // only handle connection change

  if ( !p_port_status->status_current.connect_status )
  { // TODO HUB Disconnection

    SUBTASK_EXIT(TUSB_ERROR_NONE);
  }

  // Acknowledge Port Connection Change
  OSAL_SUBTASK_INVOKED_AND_WAIT( hub_port_clear_feature_subtask(HUB_FEATURE_PORT_CONNECTION_CHANGE), error );
  SUBTASK_ASSERT_STATUS( error );

  //--------------------------------------------------------------------+
  // PORT RESET & WAIT FOR STATUS ENDPOINT & GET STATUS & CLEAR RESET CHANGE
  //--------------------------------------------------------------------+
  //------------- Set Port Reset -------------//
  OSAL_SUBTASK_INVOKED_AND_WAIT(
      usbh_control_xfer_subtask( usbh_devices[0].hub_addr, bm_request_type(TUSB_DIR_HOST_TO_DEV, TUSB_REQUEST_TYPE_CLASS, TUSB_REQUEST_RECIPIENT_OTHER),
                                 HUB_REQUEST_SET_FEATURE, HUB_FEATURE_PORT_RESET, usbh_devices[0].hub_port,
                                 0, NULL ),
      error
  );
  SUBTASK_ASSERT_STATUS( error );

  osal_task_delay(200); // TODO Hub wait for Status Endpoint on Reset Change

  //------------- Get Port Status to check if port is enabled, powered and reset_change -------------//
  OSAL_SUBTASK_INVOKED_AND_WAIT(
      usbh_control_xfer_subtask( usbh_devices[0].hub_addr, bm_request_type(TUSB_DIR_DEV_TO_HOST, TUSB_REQUEST_TYPE_CLASS, TUSB_REQUEST_RECIPIENT_OTHER),
                                 HUB_REQUEST_GET_STATUS, 0, usbh_devices[0].hub_port,
                                 4, hub_enum_buffer ),
      error
  );
  SUBTASK_ASSERT_STATUS( error );

  p_port_status = (hub_port_status_response_t *) hub_enum_buffer;
  SUBTASK_ASSERT ( p_port_status->status_change.reset && p_port_status->status_current.connect_status &&
                   p_port_status->status_current.port_power && p_port_status->status_current.port_enable);

  usbh_devices[0].speed = (p_port_status->status_current.high_speed_device_attached) ? TUSB_SPEED_HIGH :
                          (p_port_status->status_current.low_speed_device_attached ) ? TUSB_SPEED_LOW  : TUSB_SPEED_FULL;

  OSAL_SUBTASK_INVOKED_AND_WAIT(
      usbh_control_xfer_subtask( usbh_devices[0].hub_addr, bm_request_type(TUSB_DIR_HOST_TO_DEV, TUSB_REQUEST_TYPE_CLASS, TUSB_REQUEST_RECIPIENT_OTHER),
                                 HUB_REQUEST_CLEAR_FEATURE, HUB_FEATURE_PORT_RESET_CHANGE, usbh_devices[0].hub_port,
                                 0, NULL ),
      error
  );
  SUBTASK_ASSERT_STATUS( error );

  OSAL_SUBTASK_END
}

//--------------------------------------------------------------------+
// CLASS-USBH API (don't require to verify parameters)
//--------------------------------------------------------------------+
void hub_init(void)
{
  memclr_(hub_data, TUSB_CFG_HOST_DEVICE_MAX*sizeof(usbh_hub_t));
}

tusb_error_t hub_open_subtask(uint8_t dev_addr, tusb_descriptor_interface_t const *p_interface_desc, uint16_t *p_length)
{
  tusb_error_t error;

  OSAL_SUBTASK_BEGIN

  // not support multiple TT yet
  if ( p_interface_desc->bInterfaceProtocol > 1 ) return TUSB_ERROR_HUB_FEATURE_NOT_SUPPORTED;

  //------------- Open Interrupt Status Pipe -------------//
  tusb_descriptor_endpoint_t const *p_endpoint = (tusb_descriptor_endpoint_t const *) descriptor_next( (uint8_t const*) p_interface_desc );
  SUBTASK_ASSERT(TUSB_DESC_TYPE_ENDPOINT == p_endpoint->bDescriptorType);
  SUBTASK_ASSERT(TUSB_XFER_INTERRUPT == p_endpoint->bmAttributes.xfer);

  hub_data[dev_addr-1].pipe_status = hcd_pipe_open(dev_addr, p_endpoint, TUSB_CLASS_HUB);
  SUBTASK_ASSERT( pipehandle_is_valid(hub_data[dev_addr-1].pipe_status) );
  hub_data[dev_addr-1].interface_number = p_interface_desc->bInterfaceNumber;

  (*p_length) = sizeof(tusb_descriptor_interface_t) + sizeof(tusb_descriptor_endpoint_t);

  //------------- Get Hub Descriptor -------------//
  OSAL_SUBTASK_INVOKED_AND_WAIT(
    usbh_control_xfer_subtask( dev_addr, bm_request_type(TUSB_DIR_DEV_TO_HOST, TUSB_REQUEST_TYPE_CLASS, TUSB_REQUEST_RECIPIENT_DEVICE),
                               HUB_REQUEST_GET_DESCRIPTOR, 0, 0,
                               sizeof(descriptor_hub_desc_t), hub_enum_buffer ),
    error
  );
  SUBTASK_ASSERT_STATUS(error);

  // only care about this field in hub descriptor
  hub_data[dev_addr-1].port_number = ((descriptor_hub_desc_t*) hub_enum_buffer)->bNbrPorts;

  //------------- Set Port_Power on all ports -------------//
  static uint8_t i;
  for(i=1; i <= hub_data[dev_addr-1].port_number; i++)
  {
    OSAL_SUBTASK_INVOKED_AND_WAIT(
      usbh_control_xfer_subtask( dev_addr, bm_request_type(TUSB_DIR_HOST_TO_DEV, TUSB_REQUEST_TYPE_CLASS, TUSB_REQUEST_RECIPIENT_OTHER),
                                 HUB_REQUEST_SET_FEATURE, HUB_FEATURE_PORT_POWER, i,
                                 0, NULL ),
      error
    );
  }

  //------------- Queue the initial Status endpoint transfer -------------//
  SUBTASK_ASSERT_STATUS ( hcd_pipe_xfer(hub_data[dev_addr-1].pipe_status, &hub_data[dev_addr-1].status_change, 1, true) );

  OSAL_SUBTASK_END
}

void hub_isr(pipe_handle_t pipe_hdl, tusb_event_t event, uint32_t xferred_bytes)
{
  usbh_hub_t * p_hub = &hub_data[pipe_hdl.dev_addr-1];

  for (uint8_t port=1; port <= p_hub->port_number; port++)
  { // TODO HUB ignore bit0 hub_status_change
    if ( BIT_TEST_(p_hub->status_change, port) )
    {
      // TODO HUB connection/disconnection will be determined in enum task --> connect change
      usbh_device_plugged_isr(usbh_devices[pipe_hdl.dev_addr].core_id, pipe_hdl.dev_addr, port);
    }
  }

  // TODO queue next transfer
}

void hub_close(uint8_t dev_addr)
{
  (void) hcd_pipe_close(hub_data[dev_addr-1].pipe_status);
  memclr_(&hub_data[dev_addr-1], sizeof(usbh_hub_t));
}



#endif
