/*
  @file usb-moded_network.c : (De)activates network depending on the network setting system.

  Copyright (C) 2011 Nokia Corporation. All rights reserved.
  Copyright (C) 2012 Jolla. All rights reserved.

  @author: Philippe De Swert <philippe.de-swert@nokia.com>
  @author: Philippe De Swert <philippe.deswert@jollamobile.com>

  This program is free software; you can redistribute it and/or
  modify it under the terms of the Lesser GNU General Public License 
  version 2 as published by the Free Software Foundation. 

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.
 
  You should have received a copy of the Lesser GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
  02110-1301 USA
*/

/*============================================================================= */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "usb_moded.h"
#include "usb_moded-network.h"
#include "usb_moded-config.h"
#include "usb_moded-log.h"
#include "usb_moded-modesetting.h"

#if CONNMAN
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#endif

const char default_interface[] = "usb0";
typedef struct ipforward_data
{
	char *dns1;
	char *dns2;
	char *nat_interface;
}ipforward_data;

static void free_ipforward_data (struct ipforward_data *ipforward)
{
  if(ipforward)
  {
	if(ipforward->dns1)
		free(ipforward->dns1);
	if(ipforward->dns2)
		free(ipforward->dns2);
	if(ipforward->nat_interface)
		free(ipforward->nat_interface);
	free(ipforward);
  }
}

static char* get_interface(struct mode_list_elem *data)
{
  char *interface = NULL;

  if(data)
  {
	if(data->network_interface)
	{	
		interface = strdup(data->network_interface);
	}
  }
  else
  	interface = (char *)get_network_interface();

  if(interface == NULL)
  {
	interface = malloc(sizeof(default_interface)*sizeof(char));
	strncpy(interface, default_interface, sizeof(default_interface));
  }

  log_debug("interface = %s\n", interface);
  return interface;
}

/**
 * Turn on ip forwarding on the usb interface
 */
static void set_usb_ip_forward(struct mode_list_elem *data, struct ipforward_data *ipforward)
{
  const char *interface, *nat_interface;
  char command[128];

  interface = get_interface(data);
  nat_interface = get_network_nat_interface();
  if(nat_interface == NULL)
	nat_interface = strdup(ipforward->nat_interface);

  write_to_file("/proc/sys/net/ipv4/ip_forward", "1");
  snprintf(command, 128, "/sbin/iptables -t nat -A POSTROUTING -o %s -j MASQUERADE", nat_interface);
  system(command);

  snprintf(command, 128, "/sbin/iptables -A FORWARD -i %s -o %s  -m state  --state RELATED,ESTABLISHED -j ACCEPT", nat_interface, interface);
  system(command);

  snprintf(command, 128, "/sbin/iptables -A FORWARD -i %s -o %s -j ACCEPT", interface, nat_interface);
  system(command);

  free((char *)interface);
  free((char *)nat_interface);
  log_debug("ipforwarding success!\n");
}

/** 
 * Remove ip forward
 */
static void clean_usb_ip_forward(void)
{
  write_to_file("/proc/sys/net/ipv4/ip_forward", "0");
  system("/sbin/iptables -F FORWARD");
}

#ifndef CONNMAN
/**
 * Read dns settings from /etc/resolv.conf
 */
static int resolv_conf_dns(struct ipforward_data *ipforward)
{
  /* TODO: implement */
  return(0);
}
#endif

/** 
  * Write udhcpd.conf
  * @ipforward : NULL if we want a simple config, otherwise include dns info etc...
  * TODO: make this conditional ip could not have changed
  */
static int write_udhcpd_conf(struct ipforward_data *ipforward, struct mode_list_elem *data)
{
  FILE *conffile;
  const char *ip, *interface; 
  char *ipstart, *ipend;
  int dot = 0, i = 0;

  conffile = fopen("/etc/udhcpd.conf", "w");
  if(conffile == NULL)
  {
	log_debug("Error creating /etc/udhcpd.conf!\n");
	return(1);
  }

  /* generate start and end ip based on the setting */
  ip = get_network_ip();
  if(ip == NULL)
  {
	ip = strdup("192.168.2.15");
  }
  ipstart = malloc(sizeof(char)*15);
  ipend = malloc(sizeof(char)*15);
  while(i < 15)
  {
        if(dot < 3)
        {
                if(ip[i] == '.')
                        dot ++;
                ipstart[i] = ip[i];
                ipend[i] = ip[i];
        }
        else
        {
                ipstart[i] = '\0';
                ipend[i] = '\0';
                break;
        }
        i++;
  }
  strcat(ipstart,"1");
  strcat(ipend, "10");

  interface = get_interface(data);
  /* print all data in the file */
  fprintf(conffile, "start\t%s\n", ipstart);
  fprintf(conffile, "end\t%s\n", ipend);
  fprintf(conffile, "interface\t%s\n", interface);
  fprintf(conffile, "option\tsubnet\t255.255.255.0\n");
  if(ipforward != NULL)
  {
	fprintf(conffile, "opt\tdns\t%s %s\n", ipforward->dns1, ipforward->dns2);
	fprintf(conffile, "opt\trouter\t%s\n", ip);
  }

  free(ipstart);
  free(ipend);
  free((char*)ip);
  free((char*)interface);
  fclose(conffile);
  log_debug("/etc/udhcpd.conf written.\n");
  return(0);
}

#ifdef CONNMAN
/**
 * Connman message handling
 */
static const char * connman_parse_manager_reply(DBusMessage *reply)
{
  DBusMessageIter iter, subiter, origiter;
  int type;
  char *service;
  
  dbus_message_iter_init(reply, &iter);
  type = dbus_message_iter_get_arg_type(&iter);
  dbus_message_iter_recurse(&iter, &subiter);
  type = dbus_message_iter_get_arg_type(&subiter);
  origiter = subiter;
  iter = subiter;
  while(type != DBUS_TYPE_INVALID)
  {

	  if(type == DBUS_TYPE_STRUCT)
	  {
		dbus_message_iter_recurse(&iter, &subiter);
		type = dbus_message_iter_get_arg_type(&subiter);
		iter = subiter;
		if(type == DBUS_TYPE_OBJECT_PATH)
		{
			dbus_message_iter_get_basic(&iter, &service);
			log_debug("service = %s\n", service);
			if(strstr(service, "cellular"))
			{
				log_debug("cellular service found!\n");
				return(strdup(service));
			}
			iter = origiter;
		}
	   }
	dbus_message_iter_next(&iter);
	type = dbus_message_iter_get_arg_type(&iter);
  }
  return(0);
}

static int connman_fill_connection_data(DBusMessage *reply, struct ipforward_data *ipforward)
{
  DBusMessageIter array_iter, dict_iter, inside_dict_iter, variant_iter;
  DBusMessageIter sub_array_iter, string_iter;
  int type;
  char *string;
  
  log_debug("Filling in dns data\n");
  dbus_message_iter_init(reply, &array_iter);
  type = dbus_message_iter_get_arg_type(&array_iter);

  dbus_message_iter_recurse(&array_iter, &dict_iter);
  type = dbus_message_iter_get_arg_type(&dict_iter);
	  
  while(type != DBUS_TYPE_INVALID)
  {
		dbus_message_iter_recurse(&dict_iter, &inside_dict_iter);
		type = dbus_message_iter_get_arg_type(&inside_dict_iter);
		if(type == DBUS_TYPE_STRING)
		{
			dbus_message_iter_get_basic(&inside_dict_iter, &string);
			//log_debug("string = %s\n", string);
			if(!strcmp(string, "Nameservers"))
			{
				//log_debug("Trying to get Nameservers");
				dbus_message_iter_next (&inside_dict_iter);
				type = dbus_message_iter_get_arg_type(&inside_dict_iter);
				dbus_message_iter_recurse(&inside_dict_iter, &variant_iter);
				type = dbus_message_iter_get_arg_type(&variant_iter);
				if(type == DBUS_TYPE_ARRAY)
				{
					dbus_message_iter_recurse(&variant_iter, &string_iter);
					type = dbus_message_iter_get_arg_type(&string_iter);
					if(type != DBUS_TYPE_STRING)
					{
						/* not online */
						return(1);
					}
					dbus_message_iter_get_basic(&string_iter, &string);
					log_debug("dns = %s\n", string);
					ipforward->dns1 = strdup(string);
					dbus_message_iter_next (&string_iter);
					dbus_message_iter_get_basic(&string_iter, &string);
					log_debug("dns2 = %s\n", string);
					ipforward->dns2 = strdup(string);
					return(0);
				}
			}
			else if(!strcmp(string, "State"))
			{
				//log_debug("Trying to get online state");
				dbus_message_iter_next (&inside_dict_iter);
				type = dbus_message_iter_get_arg_type(&inside_dict_iter);
				dbus_message_iter_recurse(&inside_dict_iter, &variant_iter);
				type = dbus_message_iter_get_arg_type(&variant_iter);
                                if(type == DBUS_TYPE_STRING)
                                {
                                        dbus_message_iter_get_basic(&variant_iter, &string);
					log_debug("Connection state = %s\n", string);
					/* if cellular not online, connect it */
					if(strcmp(string, "online"))
					{
						log_debug("Not online. Turning on cellular data connection.\n");
						return(1);
					}
					
				}

			}
			else if(!strcmp(string, "Ethernet"))
			{
				dbus_message_iter_next (&inside_dict_iter);
				type = dbus_message_iter_get_arg_type(&inside_dict_iter);
				dbus_message_iter_recurse(&inside_dict_iter, &variant_iter);
				type = dbus_message_iter_get_arg_type(&variant_iter);
				if(type == DBUS_TYPE_ARRAY)
				{
					dbus_message_iter_recurse(&variant_iter, &sub_array_iter);
					/* we want the second dict */
					dbus_message_iter_next(&sub_array_iter);
					/* we go into the dict and get the string */
					dbus_message_iter_recurse(&sub_array_iter, &variant_iter);
					type = dbus_message_iter_get_arg_type(&variant_iter);
					if(type == DBUS_TYPE_STRING)
					{
						dbus_message_iter_get_basic(&variant_iter, &string);
						if(!strcmp(string, "Interface"))
						{
							/* get variant and iter down in it */
							dbus_message_iter_next(&variant_iter);
							dbus_message_iter_recurse(&variant_iter, &string_iter);
							dbus_message_iter_get_basic(&string_iter, &string);
							log_debug("cellular interface = %s\n", string);
							ipforward->nat_interface = strdup(string);
						}
					}
				}

			}
		}
	dbus_message_iter_next (&dict_iter);
	type = dbus_message_iter_get_arg_type(&dict_iter);
  }
  return(0);
}

/**
 * Turn on cellular connection if it is not on 
 */
static int connman_set_cellular_online(DBusConnection *dbus_conn_connman, const char *service)
{
  DBusMessage *msg = NULL;
  DBusError error;
  int ret = 0;

  dbus_error_init(&error);

  if ((msg = dbus_message_new_method_call("net.connman", service, "net.connman.Service", "Connect")) != NULL)
  {
	/* we don't care for the reply, which is empty anyway if all goes well */
        ret = !dbus_connection_send(dbus_conn_connman, msg, NULL);
	/* sleep for the connection to come up */
	sleep(3);
	/* make sure the message is sent before cleaning up and closing the connection */
	dbus_connection_flush(dbus_conn_connman);
        dbus_message_unref(msg);
  }

  return(ret);
}

static int connman_get_connection_data(struct ipforward_data *ipforward)
{
  DBusConnection *dbus_conn_connman = NULL;
  DBusMessage *msg = NULL, *reply = NULL;
  DBusError error;
  const char *service = NULL;
  int online = 0, ret = 0;

  dbus_error_init(&error);

  if( (dbus_conn_connman = dbus_bus_get(DBUS_BUS_SYSTEM, &error)) == 0 )
  {
         log_err("Could not connect to dbus for connman\n");
  }

  /* get list of services so we can find out which one is the cellular */
  if ((msg = dbus_message_new_method_call("net.connman", "/", "net.connman.Manager", "GetServices")) != NULL)
  {
        if ((reply = dbus_connection_send_with_reply_and_block(dbus_conn_connman, msg, -1, NULL)) != NULL)
        {
	    service = connman_parse_manager_reply(reply);
            dbus_message_unref(reply);
        }
        dbus_message_unref(msg);
  }
  
  log_debug("service = %s\n", service);
  if(service)
  {
try_again:
	if ((msg = dbus_message_new_method_call("net.connman", service, "net.connman.Service", "GetProperties")) != NULL)
	{
		if ((reply = dbus_connection_send_with_reply_and_block(dbus_conn_connman, msg, -1, NULL)) != NULL)
		{
			if(connman_fill_connection_data(reply, ipforward))
			{
				if(!connman_set_cellular_online(dbus_conn_connman, service) && !online)
				{
					online = 1;
					goto try_again;
				}
				else
				{
					log_debug("Cannot connect to cellular data\n");
					ret = 1;
				}
			}
			dbus_message_unref(reply);
		}
		dbus_message_unref(msg);
	}
  }
  dbus_connection_unref(dbus_conn_connman);
  dbus_error_free(&error);
  free((char *)service);
  return(ret);
}
#endif /* CONNMAN */

/** 
 * Write out /etc/udhcpd.conf conf so the config is available when it gets started
 */
int usb_network_set_up_dhcpd(struct mode_list_elem *data)
{
  struct ipforward_data *ipforward = NULL;

  /* Set up nat info only if it is required */
  if(data->nat)
  {
	ipforward = malloc(sizeof(struct ipforward_data));
#ifdef CONNMAN
	if(connman_get_connection_data(ipforward))
	{
		log_debug("data connection not available!\n");
		/* TODO: send a message to the UI */
		goto end;
	}
#else
	  resolv_conf_dns(ipforward);
#endif /*CONNMAN */
  }
  /* ipforward can be NULL here, which is expected and handled in this function */
  write_udhcpd_conf(ipforward, data);

  if(data->nat)
	set_usb_ip_forward(data, ipforward);


end:
  /* the function checks if ipforward is NULL or not */
  free_ipforward_data(ipforward);
  return(0);
}

/**
 * Activate the network interface
 *
 */
int usb_network_up(struct mode_list_elem *data)
{
  const char *ip, *interface, *gateway;
  char command[128];
  int ret = -1;

#if CONNMAN_IS_EVER_FIXED_FOR_USB
  DBusConnection *dbus_conn_connman = NULL;
  DBusMessage *msg = NULL, *reply = NULL;
  DBusError error;

  dbus_error_init(&error);

  if( (dbus_conn_connman = dbus_bus_get(DBUS_BUS_SYSTEM, &error)) == 0 )
  {
         log_err("Could not connect to dbus for connman\n");
  }

  if ((msg = dbus_message_new_method_call("net.connman", "/", "net.connman.Service", connect)) != NULL)
  {
        if ((reply = dbus_connection_send_with_reply_and_block(dbus_conn_connman, msg, -1, NULL)) != NULL)
        {
            dbus_message_get_args(reply, NULL, DBUS_TYPE_INT32, &ret, DBUS_TYPE_INVALID);
            dbus_message_unref(reply);
        }
        dbus_message_unref(msg);
  }
  dbus_connection_unref(dbus_conn_connman);
  dbus_error_free(&error);
  return(ret);

#else

  interface = get_interface(data); 
  ip = get_network_ip();
  gateway = get_network_gateway();

  if(ip == NULL)
  {
	sprintf(command,"ifconfig %s 192.168.2.15", interface);
	system(command);
	goto clean;
  }
  else if(!strcmp(ip, "dhcp"))
  {
	sprintf(command, "dhclient -d %s\n", interface);
	ret = system(command);
	if(ret != 0)
	{	
		sprintf(command, "udhcpc -i %s\n", interface);
		system(command);
	}

  }
  else
  {
	sprintf(command, "ifconfig %s %s\n", interface, ip);
	system(command);
  }

  /* TODO: Check first if there is a gateway set */
  if(gateway)
  {
	sprintf(command, "route add default gw %s\n", gateway);
        system(command);
  }

clean:
  free((char *)interface);
  free((char *)gateway);
  free((char *)ip);

  return(0);
#endif /* CONNMAN_IS_EVER_FIXED_FOR_USB */
}

/**
 * Deactivate the network interface
 *
 */
int usb_network_down(struct mode_list_elem *data)
{
#if CONNMAN_IS_EVER_FIXED_FOR_USB
#else
  const char *interface;
  char command[128];

  interface = get_interface(data);

  sprintf(command, "ifconfig %s down\n", interface);
  system(command);

  /* dhcp client shutdown happens on disconnect automatically */
  if(data->nat)
	clean_usb_ip_forward();

  free((char *)interface);
  
  return(0);
#endif /* CONNMAN_IS_EVER_FIXED_FOR_USB */
}

/**
 * Update the network interface with the new setting if connected.
 * 
*/
int usb_network_update(void)
{
  struct mode_list_elem * data;

  if(!get_usb_connection_state())
	return(0);

  data = get_usb_mode_data();
  if(data->network)
  {
	usb_network_down(data);	
	usb_network_up(data);	
	return(0);
  }
  else
	return(0);

}
