#include <jni.h>
#include <stdio.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>

#include <sys/un.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <netinet/udp.h>
#include <netinet/ip.h>
#include <unistd.h>
#include <inttypes.h>
#include <time.h>

#include "onep_core_services.h"
#include "policy.h"
#include "datapath_policy.h"
#include "onep_types.h"
#include "filter.h"
#include "onep_dpss_packet_delivery.h"
#include "onep_dpss_pkt.h"
#include "onep_dpss_flow.h"
#include "onep_dpss_actions.h"
#include "onep_dpss_callback_framework.h"

#include "datapath_NodePuppet.h"
 
#define TRY(_rc, _expr, _env, _jobj, _errFid, _fmt, _args...)                                 \
if (((_rc) = (_expr)) != ONEP_OK) {                                 \
char *_tmpErrBuf = g_strdup_printf("\n%s:%d: Error: %s(%d): " _fmt "\n",               \
__FILE__, __LINE__, onep_strerror((_rc)), (_rc) , ##_args); \
jstring _errBuf = (*_env)->NewStringUTF(_env, _tmpErrBuf); \
g_free(_tmpErrBuf); \
(*_env)->SetObjectField(_env, _jobj, _errFid, _errBuf); \
return ((_rc));                                                 \
}

typedef struct list{
	struct list* next;
	struct list* previous;
	uint16_t data;//16 bit ip header identification field
	time_t timestamp;
} List;

static List *root = NULL;
static int timeout = 10; //how long we wait before declaring a packet loss


static void add_to_end(List *list, uint16_t _data, time_t _timestamp){
	if(list == NULL){
		root = (List *)malloc(sizeof(List));
		root->data = _data;
		root->timestamp = _timestamp;
		root->next = NULL;
		root->previous = NULL;
		printf("added packet at beginnging with id %d to end at time %ld\n", _data, _timestamp);
		return;
	}
	List* last = list;
	while (1){
		if(last->next == NULL){
			break;
		}else{
			last = last->next;
		}
	}
	last->next = malloc(sizeof(List));
	last->next->previous = last;
	last->next->data = _data;
	last->next->timestamp = _timestamp;
	last->next->next = NULL;
	printf("added packet with id %d to end at time %ld\n", _data, _timestamp);
}

static void print_list(List *list){
	printf("------------------------------List-Contents--------------------------\n");
	if(list == NULL){
		printf("List is empty\n");
		printf("---------------------------------------------------------------------\n");
		return;
	}
	while(list->next != NULL){
		printf("ID:%d   Time Lapse:%d\n", list->data, (int) (time(NULL) - list->timestamp));
		list = list->next;
	}
	printf("ID:%d   Time Lapse:%d\n", list->data, (int)(time(NULL) - list->timestamp));
	printf("---------------------------------------------------------------------\n");
}

static int search_and_remove(List *list, uint16_t _data){
	if(list == NULL)return 0;
	if(list->data == _data){
		root = list->next;
		free(list);
		printf("found and removed packet with id %d\n", _data);
		return 1;
	}else{
		while(1){
			if(time(NULL) - list->timestamp > timeout){
				printf("PACKET LOSS: packet with id %d never seen\n", list->data);
				root = list->next;
				free(list);
				list = root;
			}else{
				break;
			}
		}
	}
	while(1){
		if(list->next == NULL){
			if(list->data == _data){
				return 1;
			}else{
				if(time(NULL) - list->timestamp > timeout) printf("PACKET LOSS\n");
				return 0;
			}
		}
		if(list->next->data == _data){
			List *temp = list->next;
			list->next = list->next->next;
			printf("found and removed packet with id %d\n", _data);
			free(temp);
			return 1;
		}else{
			if(time(NULL) - list->next->timestamp > timeout){
				printf("PACKETT LOSS: packet with id %d never seen\n", list->next->data);
				List *temp = list->next;
				list->next = list->next->next;
				free(temp);
			}
			list = list->next;
		}
	}
}

static void dpss_tutorial_display_intf_list(onep_collection_t *intf_list, FILE *op)
{
    onep_status_t rc;
    unsigned int count;
    network_interface_t* intf;
    onep_if_name name;

    onep_collection_get_size(intf_list, &count);
    if (count>0) {
        unsigned int i;
        for (i = 0; i < count; i++) {
            rc = onep_collection_get_by_index(intf_list, i, (void *)&intf);
            if (rc==ONEP_OK) {
                rc = onep_interface_get_name(intf,name);
                fprintf(op, "[%d] Interface [%s]\n", i, name);
            } else {
            	fprintf(stderr, "Error getting interface. code[%d], text[%s]\n", rc, onep_strerror(rc));
            }
        }
    }
}

static onep_status_t dpss_tutorial_get_ip_version(struct onep_dpss_paktype_ *pakp,
    char *ip_version) {

    onep_status_t rc;
    uint16_t l3_protocol;
    char l3_prot_sym = 'U';

    /* Get packet L3 protocol. */
    rc = onep_dpss_pkt_get_l3_protocol(pakp, &l3_protocol);
    if( rc == ONEP_OK ) {
        if( l3_protocol == ONEP_DPSS_L3_IPV4 ) {
            l3_prot_sym = '4';
        } else if( l3_protocol == ONEP_DPSS_L3_IPV6 ) {
            l3_prot_sym = '6';
        } else if( l3_protocol == ONEP_DPSS_L3_OTHER ) {
            l3_prot_sym = 'N';
        } else {
            l3_prot_sym = 'U';
        }
    } else {
        fprintf(stderr, "Error getting L3 protocol. code[%d], text[%s]\n", rc, onep_strerror(rc));
        return (rc);
    }
    *ip_version = l3_prot_sym;
    return (ONEP_OK);
}


/*
 * Extract IP addressing and port information from the packet.
 */
static onep_status_t dpss_tutorial_get_ip_port_info(
    struct onep_dpss_paktype_ *pakp, char **src_ip, char **dest_ip,
    uint16_t *src_port, uint16_t *dest_port, char *prot, char ip_version, u_int16_t *pkt_id) {

    onep_status_t   rc;
    uint8_t         l4_protocol;
    uint8_t         *l3_start;
    struct iphdr    *l3hdr;
    uint8_t         *l4_start;
    struct tcphdr   *l4tcp;
    struct udphdr   *l4udp;

    uint8_t 		*payload_start;

    onep_dpss_pkt_get_payload(pakp, &payload_start);

    if( ip_version == '4' ) {
    	/*get payload */
    	/* get IPv4 header */
        rc = onep_dpss_pkt_get_l3_start(pakp, &l3_start);
        if( rc==ONEP_OK ) {
            l3hdr = (struct iphdr *)l3_start; // convert to iphdr
            *src_ip = strdup(inet_ntoa( *(struct in_addr *)&(l3hdr->saddr) ));
            *dest_ip = strdup(inet_ntoa( *(struct in_addr *)&(l3hdr->daddr) ));
            *pkt_id = ntohs(l3hdr->id);
        } else {
            fprintf(stderr,"Error getting IPv4 header. code[%d], text[%s]\n", rc, onep_strerror(rc));
            return (ONEP_ERR_SYSTEM);
        }
    } else if( ip_version == '6' ) {
        fprintf(stderr, "Cannot get IPv6 traffic at this time.\n");
        return (ONEP_ERR_SYSTEM);
    } else if( ip_version == 'N' ) {
        fprintf(stderr, "IP address is neither IPv4 nor IPv6.\n");
        return (ONEP_ERR_SYSTEM);
    } else {
        fprintf(stderr, "Unknown IP version.\n");
        return (ONEP_ERR_SYSTEM);
    }

    /* get L4 header */
    rc = onep_dpss_pkt_get_l4_start(pakp, &l4_start);
    if( rc != ONEP_OK ) {
        fprintf(stderr, "Error getting L4 header. code[%d], text[%s]\n", rc, onep_strerror(rc));
        return (rc);
    }

    /* get packet L4 protocol */
    rc = onep_dpss_pkt_get_l4_protocol(pakp, &l4_protocol);
    if( rc == ONEP_OK ) {
        if( l4_protocol == ONEP_DPSS_TCP_PROT ) {
            /* TCP */
            strcpy(prot,"TCP");
            l4tcp = (struct tcphdr *)l4_start;
            *src_port = ntohs( l4tcp->source );
            *dest_port = ntohs( l4tcp->dest );
        }
        else if( l4_protocol == ONEP_DPSS_UDP_PROT ) {
            /* UDP */
            strcpy(prot,"UDP");
            l4udp = (struct udphdr *)l4_start;
            *src_port = ntohs( l4udp->source );
            *dest_port = ntohs( l4udp->dest );
        }
        else if( l4_protocol == ONEP_DPSS_ICMP_PROT ) {
            strcpy(prot,"ICMP");
        }
        else if( l4_protocol == ONEP_DPSS_IPV6_ENCAPSULATION_PROT ) {
            // sends IPV6 packet as payload of IPV4
            strcpy(prot,"ENCP"); // IPV6 encapsulated on IPV4
        }
        else {
            strcpy(prot,"UNK!"); // Unknown!
        }
    }
    else {
        fprintf(stderr, "Error getting L4 protocol. code[%d], text[%s]\n", rc, onep_strerror(rc));
    }
    return (ONEP_OK);
}


/*
 * Extract some flow state given a packet and a FID.
 */
static void dpss_tutorial_get_flow_state(struct onep_dpss_paktype_ *pakp,
    onep_dpss_flow_ptr_t fid, char *l4_state_char ) {

    onep_status_t             rc;
    onep_dpss_l4_flow_state_e l4_state;

    rc = onep_dpss_flow_get_l4_flow_state(pakp,&l4_state);
    if( rc==ONEP_OK ) {
        if( l4_state == ONEP_DPSS_L4_CLOSED ) {
            strcpy(l4_state_char,"CLOSED");
        } else if( l4_state == ONEP_DPSS_L4_OPENING ) {
            strcpy(l4_state_char,"OPENING");
        } else if( l4_state == ONEP_DPSS_L4_UNI_ESTABLISHED ) {
            strcpy(l4_state_char,"UNI-ESTABLISHED");
        } else if( l4_state == ONEP_DPSS_L4_UNI_ESTABLISHED_INCORRECT ) {
            strcpy(l4_state_char,"UNI-ESTABLISHED INCORRECT");
        } else if( l4_state == ONEP_DPSS_L4_BI_ESTABLISHED ) {
            strcpy(l4_state_char,"BI-ESTABLISHED");
        } else if( l4_state == ONEP_DPSS_L4_BI_ESTABLISHED_INCORRECT ) {
            strcpy(l4_state_char,"BI-ESTABLISHED INCORRECT");
        } else if( l4_state == ONEP_DPSS_L4_CLOSING ) {
            strcpy(l4_state_char,"CLOSING");
        } else {
            strcpy(l4_state_char,"!UNKNOWN!");
        }
    } else {
        fprintf(stderr, "Error getting L4 state of flow. code[%d], text[%s]\n", rc, onep_strerror(rc));
    }
    return;
}

static void
disconnect_network_element (network_element_t **ne,
                            session_handle_t **session_handle)
{
    network_application_t* myapp = NULL;
    onep_status_t rc;

    if ((ne) && (*ne)) {
        /* Done with Network Element, disconnect it. */
        rc = onep_element_disconnect(*ne);
        if (rc != ONEP_OK) {
            fprintf(stderr, "\nFail to disconnect network element:"
                    " errocode = %d, errormsg = %s",
                     rc, onep_strerror(rc));
        }
        /* Free the network element resource on presentation. */
        rc = onep_element_destroy(ne);
        if (rc != ONEP_OK) {
            fprintf(stderr, "\nFail to destroy network element:"
                    " errocode = %d, errormsg = %s",
                     rc, onep_strerror(rc));
        }
    }
    /* Free the onePK resource on presentation. */
    if (session_handle) {
        rc = onep_session_handle_destroy(session_handle);
        if (rc != ONEP_OK) {
            fprintf(stderr, "\nFail to destroy session handle:"
                    " errocode = %d, errormsg = %s",
                     rc, onep_strerror(rc));
        }
    }
    /* Gets the singleton instance of network_application_t. */
    rc = onep_application_get_instance(&myapp);
    if (rc != ONEP_OK) {
        fprintf(stderr, "\nFail to get the instance of the application:"
                " errocode = %d, errormsg = %s",
                 rc, onep_strerror(rc));
    }
    if (myapp) {
        /* Destroys the network_application_t and frees its memory resource. */
        rc = onep_application_destroy(&myapp);
        if (rc != ONEP_OK) {
            fprintf(stderr, "\nFail to destroy application:"
                    " errocode = %d, errormsg = %s",
                     rc, onep_strerror(rc));
        }
    }
}
static onep_status_t create_in_acl( network_element_t 	*elm,
									class_t 			**in_class,
									int					proto,
									int					port){
	onep_status_t rc;
	ace_t *our_ace;
	acl_t *our_acl;
	filter_t* acl_filter_in;
	class_t* acl_class_in;

	/* Create the traffic class */

	// Create ACE
	rc = onep_acl_create_l3_ace(40, TRUE, &our_ace);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to create l3 ace: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}
	// Set the source prefix
	rc = onep_acl_set_l3_ace_src_prefix(our_ace, NULL, 0);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to set source prefix: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}
	// Set the destination prefix
	rc = onep_acl_set_l3_ace_dst_prefix(our_ace, NULL, 0);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to set dest prefix: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}
	// Set the protocol
	rc = onep_acl_set_l3_ace_protocol(our_ace, proto);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to set protocol: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}
	// Set the source port
	rc = onep_acl_set_l3_ace_src_port(our_ace, 0, ONEP_COMPARE_ANY);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to set source port: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}
	// Set the destination port
	rc = onep_acl_set_l3_ace_dst_port(our_ace, port, ONEP_COMPARE_EQ);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to set dest port: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}
	/* Now create the related ACL.  After creating the ACL we will add ace40 to it*/
	rc = onep_acl_create_l3_acl(AF_INET, elm, &our_acl);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to create acl: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}

	rc = onep_acl_add_ace(our_acl, our_ace);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to add ace to acl: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}
	printf("\n Added ace to acl\n");

	/* Now that the ACL is created, we can create a class map with an ACL filter */
	rc = onep_policy_create_class(elm, ONEP_CLASS_OPER_OR, &acl_class_in);
	if (rc != ONEP_OK) {
		 fprintf(stderr, "Unable to create class: %s\n", onep_strerror(rc));
		 return ONEP_FAIL;
	}

	/* Create an acl filter containing the acl created above*/
	rc = onep_policy_create_acl_filter(our_acl, &acl_filter_in);
	if (rc != ONEP_OK) {
		   fprintf(stderr, "Unable to create acl filter: %s\n", onep_strerror(rc));
		   return ONEP_FAIL;
	}

	/* Now add the ACL filter to the created acl_class*/
	rc = onep_policy_add_class_filter(acl_class_in, acl_filter_in);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to add filter to class: %s\n",
			onep_strerror(rc));
		return ONEP_FAIL;
	}
	/*Assuming we got this far, we want to return the class we created */
	*in_class = acl_class_in;

	return (ONEP_OK);
}

static onep_status_t create_out_acl( network_element_t 	*elm,
									class_t 			**out_class,
									int					proto,
									int					port){
	onep_status_t rc;
	ace_t *our_ace;
	acl_t *our_acl;
	filter_t* acl_filter_out;
	class_t* acl_class_out;

	/* Create the traffic class */

	// Create ACE
	rc = onep_acl_create_l3_ace(50, TRUE, &our_ace);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to create l3 ace: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}
	// Set the source prefix
	rc = onep_acl_set_l3_ace_src_prefix(our_ace, NULL, 0);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to set source prefix: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}
	// Set the destination prefix
	rc = onep_acl_set_l3_ace_dst_prefix(our_ace, NULL, 0);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to set dest prefix: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}
	// Set the protocol
	rc = onep_acl_set_l3_ace_protocol(our_ace, proto);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to set protocol: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}
	// Set the source port
	rc = onep_acl_set_l3_ace_src_port(our_ace, port, ONEP_COMPARE_EQ);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to set source port: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}
	// Set the destination port
	rc = onep_acl_set_l3_ace_dst_port(our_ace, 0, ONEP_COMPARE_ANY);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to set dest port: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}
	/* Now create the related ACL.  After creating the ACL we will add ace40 to it */
	rc = onep_acl_create_l3_acl(AF_INET, elm, &our_acl);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to create acl: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}

	rc = onep_acl_add_ace(our_acl, our_ace);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to add ace to acl: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}
	printf("\n Added ace to acl\n");

	/* Now that the ACL is created, we can create a class map with an ACL filter */
	rc = onep_policy_create_class(elm, ONEP_CLASS_OPER_OR, &acl_class_out);
	if (rc != ONEP_OK) {
		 fprintf(stderr, "Unable to create class: %s\n", onep_strerror(rc));
		 return ONEP_FAIL;
	}

	/* Create an acl filter containing the acl created above */
	rc = onep_policy_create_acl_filter(our_acl, &acl_filter_out);
	if (rc != ONEP_OK) {
		   fprintf(stderr, "Unable to create acl filter: %s\n", onep_strerror(rc));
		   return ONEP_FAIL;
	}

	/* Now add the ACL filter to the created acl_class */
	rc = onep_policy_add_class_filter(acl_class_out, acl_filter_out);
	if (rc != ONEP_OK) {
			fprintf(stderr, "Unable to add filter to class: %s\n",
				onep_strerror(rc));
			return ONEP_FAIL;
	}
    /* Assuming we got this far, we want to return the class we created */
	*out_class = acl_class_out;

	return (ONEP_OK);
}

static void out_packet_drop_callback( onep_dpss_traffic_reg_t *reg, struct onep_dpss_paktype_ *pak, void *client_context, bool *return_packet){
	onep_status_t        rc;
    onep_dpss_fid_t      fid;
    uint16_t             src_port = 0;
    uint16_t             dest_port = 0;
    char                 *src_ip = NULL;
    char                 *dest_ip = NULL;
    char                 l4_protocol[5];
    char                 l4_state[30];

    /* TODO: MY VARS */
    uint16_t			pkt_id = 0;
    network_interface_t* input_int;
    network_interface_t* output_int;
    onep_if_name 		 input;
    onep_if_name 		 output;
    /* END MY VARS */

    strcpy(l4_protocol,"ERR");
    strcpy(l4_state,"ERR");

    rc = onep_dpss_pkt_get_flow(pak, &fid);
    if( rc == ONEP_OK ) {
    	dpss_tutorial_get_ip_port_info(pak, &src_ip, &dest_ip, &src_port, &dest_port, l4_protocol, '4', &pkt_id);
    	dpss_tutorial_get_flow_state(pak, fid, l4_state);

        /*TODO: MY CODE */
        //Get input and output interface of packet
        onep_dpss_pkt_get_input_interface(pak, &input_int);
        onep_dpss_pkt_get_output_interface(pak, &output_int);

        //Get names of interfaces
        onep_interface_get_name(input_int, input);
        onep_interface_get_name(output_int, output);
        /* END MY CODE */

    } else {
        fprintf(stderr, "Error getting flow ID. code[%d], text[%s]\n", rc, onep_strerror(rc));
    }
    printf("\n"
    		"Out - %-4d | %-18s | %-15s (%-5d) --> %-15s (%-5d)\n", pkt_id, output, src_ip, src_port, dest_ip, dest_port);
    search_and_remove(root, pkt_id);
    //print_list(root);
    free(src_ip);
    free(dest_ip);
    return;
}
static void in_packet_drop_callback( onep_dpss_traffic_reg_t *reg,
							  struct onep_dpss_paktype_ *pak,
							  void *client_context,
							  bool *return_packet){

		onep_status_t        rc;
	    onep_dpss_fid_t      fid;
	    uint16_t             src_port = 0;
	    uint16_t             dest_port = 0;
	    char                 *src_ip = NULL;
	    char                 *dest_ip = NULL;
	    char                 l4_protocol[5];
	    char                 l4_state[30];

	    /* TODO: MY VARS */
	    uint16_t			pkt_id = 0;
	    network_interface_t* input_int;
	    network_interface_t* output_int;
	    onep_if_name 		 input;
	    onep_if_name 		 output;


	    /* END MY VARS */

	    strcpy(l4_protocol,"ERR");
	    strcpy(l4_state,"ERR");

	    rc = onep_dpss_pkt_get_flow(pak, &fid);
	    if( rc == ONEP_OK ) {
	    	dpss_tutorial_get_ip_port_info(pak, &src_ip, &dest_ip, &src_port, &dest_port, l4_protocol, '4', &pkt_id);
	    	dpss_tutorial_get_flow_state(pak, fid, l4_state);

	        /*TODO: MY CODE */
	        //Get input and output interface of packet
	        onep_dpss_pkt_get_input_interface(pak, &input_int);
	        onep_dpss_pkt_get_output_interface(pak, &output_int);

	        //Get names of interfaces
	        onep_interface_get_name(input_int, input);
	        onep_interface_get_name(output_int, output);
	        /* END MY CODE */


	    } else {
	        fprintf(stderr, "Error getting flow ID. code[%d], text[%s]\n", rc, onep_strerror(rc));
	    }

	    printf("\n"
	    		"In  - %-4d | %-18s | %-15s (%-5d) --> %-15s (%-5d)\n", pkt_id, input, src_ip, src_port, dest_ip, dest_port);
	    add_to_end(root, pkt_id, time(NULL));
	    //print_list(root);
	    free(src_ip);
	    free(dest_ip);
	    return;
}

static onep_status_t register_traffic(network_element_t *ne,
									  onep_if_name interface_name,
									  class_t *in_acl,
									  class_t *out_acl,
									  onep_dpss_traffic_reg_t **in_handle,
									  onep_dpss_traffic_reg_t **out_handle){

	onep_status_t rc;
	network_interface_t *this_interface;
	target_t *in_target;
	target_t *out_target;
	onep_dpss_traffic_reg_t *in_handle_temp;
	onep_dpss_traffic_reg_t *out_handle_temp;

	//Get the
    rc = onep_element_get_interface_by_name(ne, interface_name, &this_interface);
    if (rc != ONEP_OK) {
        fprintf(stderr, "Error in getting interface: %s\n", onep_strerror(rc));
        return ONEP_FAIL;
    }

    //Interface targets for int1
    rc = onep_policy_create_interface_target(this_interface, ONEP_TARGET_LOCATION_HARDWARE_DEFINED_INPUT, &in_target);
    if (rc != ONEP_OK) {
        fprintf(stderr, "Error creating target interface: %s\n", onep_strerror(rc));
        return ONEP_FAIL;
    }
    rc = onep_policy_create_interface_target(this_interface, ONEP_TARGET_LOCATION_HARDWARE_DEFINED_OUTPUT, &out_target);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Error creating target interface: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}
	//Register for packets
	rc = onep_dpss_register_for_packets(in_target, in_acl, ONEP_DPSS_ACTION_COPY, in_packet_drop_callback, 0, &in_handle_temp);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to register for packets: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}
	rc = onep_dpss_register_for_packets(out_target, out_acl, ONEP_DPSS_ACTION_COPY, out_packet_drop_callback, 0, &out_handle_temp);
	if (rc != ONEP_OK) {
		fprintf(stderr, "Unable to register for packets: %s\n", onep_strerror(rc));
		return ONEP_FAIL;
	}

	/* If we made it here, we registered successfully */
	*in_handle = in_handle_temp;
	*out_handle = out_handle_temp;
	return ONEP_OK;

}

JNIEXPORT int JNICALL Java_datapath_NodePuppet_ProgramNode(JNIEnv *env, jobject thisObj, jstring j_address, jstring j_user, jstring j_pass, jstring j_protocol) {
	network_application_t* myapp = NULL;
	//network_element_t*     local_ne = NULL;
	session_handle_t*      session_handle = NULL;
	onep_status_t          rc;
	struct sockaddr_in     v4addr;
	//onep_transport_mode_e  mode;
	session_config_t*      config = NULL;
	network_element_t *ne1;
    char *c_address, *c_username, *c_password, *c_protocol;

    	class_t* acl_class_in;
    	class_t* acl_class_out;
    	interface_filter_t* intf_filter = NULL;
    	onep_collection_t*  intfs = NULL;
		network_interface_t* intf;
    	unsigned int        count = 0;
    	onep_if_name        intf_name;
    	onep_dpss_traffic_reg_t *in_handle;
    	onep_dpss_traffic_reg_t *out_handle;
    	uint64_t pak_count, last_pak_count;


	/*Get a reference to this object's class */
	jclass thisClass = (*env)->GetObjectClass(env, thisObj);


	jfieldID errFid;
	errFid = (*env)->GetFieldID(env, thisClass, "errBuf", "Ljava/lang/String;");
	    if (errFid == NULL) {
	        fprintf(stderr, "Failed to find field errBuf!\n");
	        return (ONEP_ERR_NO_DATA);
	    }


	/* Create Application instance. */
		TRY(rc, onep_application_get_instance(&myapp), env, thisObj, errFid,
		"onep_application_get_instance");
		onep_application_set_name(myapp, "Program Node");

	/* Set session parameters */
		TRY(rc, onep_session_config_new(ONEP_SESSION_SOCKET, &config), env, thisObj, errFid,
		"onep_session_config_new");
		onep_session_config_set_event_queue_size(config, 300);
		onep_session_config_set_event_drop_mode(config, ONEP_SESSION_EVENT_DROP_OLD);

	/* Get arguments */
		c_address 	= (char *) (*env)->GetStringUTFChars(env, j_address, NULL);
		c_username 	= (char *) (*env)->GetStringUTFChars(env, j_user, NULL);
		c_password 	= (char *) (*env)->GetStringUTFChars(env, j_pass, NULL);
		c_protocol 	= (char *) (*env)->GetStringUTFChars(env, j_protocol, NULL);

		printf("Address: %s Username: %s Password: %s Protocol: %s\n", c_address, c_username, c_password, c_protocol);

	/* Set address of Network Element */
		memset(&v4addr, 0, sizeof(struct sockaddr_in));
		v4addr.sin_family = AF_INET;
		inet_pton(AF_INET, c_address, &(v4addr.sin_addr));
		TRY(rc, onep_application_get_network_element(
		myapp, (struct sockaddr*)&v4addr, &ne1), env, thisObj, errFid,
		"onep_application_get_network_element");

	/* Connect to Network Element */
		TRY(rc, onep_element_connect(ne1, c_username, c_password, config, &session_handle), env, thisObj, errFid,
		"onep_element_connect");

		if (!session_handle) {
			fprintf(stderr, "\n*** create_network_connection fails ***\n");
			return ONEP_FAIL;
		}
		printf("\n Network Element CONNECT SUCCESS \n");

	/* Create incoming ACL */
		rc = create_in_acl(ne1, &acl_class_in, 6, 80);
		if (rc != ONEP_OK) {
			fprintf(stderr, "\nCannot turn on interface"
					"code[%d], text[%s]\n", rc, onep_strerror(rc));
			goto cleanup;
		}

	/* Create outgoing ACL */
		rc = create_out_acl(ne1, &acl_class_out, 6, 80);
		if (rc != ONEP_OK) {
			fprintf(stderr, "\nCannot turn on interface"
					"code[%d], text[%s]\n", rc, onep_strerror(rc));
			goto cleanup;
		}

	/* Get list of interfaces on device, then find the interface we want */
		onep_interface_filter_new(&intf_filter);
		onep_interface_filter_set_type(intf_filter, ONEP_IF_TYPE_ETHERNET);
		rc = onep_element_get_interface_list(ne1, intf_filter, &intfs);
			if (rc != ONEP_OK) {
				fprintf(stderr, "\nError getting interface. code[%d], text[%s]\n", rc, onep_strerror(rc));
				goto cleanup;
			}
	/* Display the interfaces we retrieved */
		dpss_tutorial_display_intf_list(intfs,stderr);
		uint32_t intf_count;


	/* Register some packet handlers */
			onep_if_name name;

			onep_collection_get_size(intfs, &intf_count);
			if (intf_count>0) {
				unsigned int i;
				/* for each interface, we will register for incoming and outgoing traffic */
				for (i = 0; i < intf_count; i++) {
					rc = onep_collection_get_by_index(intfs, i, (void *)&intf);
					if (rc==ONEP_OK) {
						rc = onep_interface_get_name(intf,name);
						fprintf(stderr, "Registering for traffic on %s\n", name);

						rc = register_traffic(ne1, name, acl_class_in, acl_class_out, &in_handle, &out_handle);
						if(rc != ONEP_OK){
							fprintf(stderr, "Problem registering for interface %s\n", name);
						}
					} else {
						fprintf(stderr, "Error getting interface. code[%d], text[%s]\n", rc, onep_strerror(rc));
					}
//					onep_dpss_traffic_reg_t *in1_handle;
//					onep_dpss_traffic_reg_t *in2_handle;
//					onep_dpss_traffic_reg_t *out1_handle;
//					onep_dpss_traffic_reg_t *out2_handle;
//				onep_if_name int1_name = "GigabitEthernet0/0";
//				onep_if_name int2_name = "GigabitEthernet0/3";
//				rc = register_traffic(ne1, int1_name, acl_class_in, acl_class_out, &in1_handle, &out1_handle);
//				rc = register_traffic(ne1, int2_name, acl_class_in, acl_class_out, &in2_handle, &out2_handle);
				}
			} if (intf_count <= 0 ) {
				fprintf(stderr, "\nNo interfaces available");
				goto cleanup;
			}

			last_pak_count = 0;
			/* wait to query the packet loop for the number
			 * of packets received and processed. */
			while (1) {
				sleep(timeout);
				(void) onep_dpss_packet_callback_rx_count(&pak_count);
				fprintf(stderr, "Current Packet Count: %lu\n", pak_count);
				if (pak_count == last_pak_count) {
				  break;
				} else {
				  last_pak_count = pak_count;
				}
			}
			 printf("done\n");

	/* END SNIPPET: register_packets */

	//the int
	//get the Field ID of number
	jfieldID fidNumber = (*env)->GetFieldID(env, thisClass, "number","I");
	if(NULL == fidNumber) return 1;

	//Get the int given the Field ID
	jint number = (*env)->GetIntField(env, thisObj, fidNumber);
	printf("In C, the int is %d\n", number);

	//Change the variable
	number = 99;
	(*env)->SetIntField(env, thisObj, fidNumber, number);


	cleanup:
			disconnect_network_element(&ne1, &session_handle);
	//At the end release the resources
    (*env)->ReleaseStringUTFChars(env, j_address, c_address);
    (*env)->ReleaseStringUTFChars(env, j_user, c_username);
    (*env)->ReleaseStringUTFChars(env, j_pass, c_password);
    (*env)->ReleaseStringUTFChars(env, j_protocol, c_protocol);  // release resources
   return 1;
}