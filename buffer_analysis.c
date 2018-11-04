/* The buffer analysis program is used to estimate several QoS      */  
/* (quality of service) metrics pertaining to the retransmission    */
/* over WiFi of an in-bound videostream. Average queuing delay,     */
/* packet loss percentage, # of incoming packets, # of dropped 		*/
/* packets, and # of delivered packets are computed, given a 		*/
/* certain outbound transmission rate from the access point; as		*/
/* well as a fixed access point buffer size.						*/
/* 														  			*/
/* Usage: (see comment above main() for more details) 				*/
/*  													  			*/
/* 	Compile: clang -g3 -O0 buffer_analysis.c -o buffer_analysis		*/
/*	Run:     ./buffer_analysis [n]	                    			*/
/*																	*/
/* Written by Daniel Tyler Gillson       		November 9, 2018 	*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <float.h>
#include <stdbool.h>
#include <errno.h>

/* Manifest constants */
int tr_out;  				// Outgoing WiFi transmission rate from AP (Mbps)
#define BYTES_TO_MB 131072  // There are 131,072 bytes in a megabit
#define OUTGOING_BYTES_PER_SECOND tr_out * BYTES_TO_MB
#define START_TIME 0.000000
#define TIME_UNIT 0.000001  // Assumption: timestamps = fractions of a second
#define LINE_LEN 56

// Global counters + other vars:
int outbound_rates[] = {11, 6, 30, 54};  // tr_out alternatives, (DEFAULT=11)
int total_pkts, delivered_pkts, dropped_pkts, buff_ctr, buff_size, id;
float processed_bytes, total_delay;
double global_time = START_TIME;
char line[LINE_LEN];
char * VIDEO_TRACE = "/Users/tylergillson/Dropbox/UofC/F2018/CPSC 441/Assignments/A3/soccer.txt";
bool debug = false;

/*
 * Source for linked list implementation:
 * https://stackoverflow.com/questions/23279119/creating-and-understanding-linked-lists-of-structs-in-c
 */

// Packet struct to record relevant statistics:
typedef struct NODE_PACKET_S {
	int num_bytes;
	int id;
	float arrival_time;
	float departure_time;
} NODE_PACKET_T;

// Linked list of packets structs:
typedef struct LIST_NODE_S {
	struct LIST_NODE_S *next;   // pointer to the next node in the list
	NODE_PACKET_T      packet;  // packet struct
} LIST_NODE_T;

LIST_NODE_T *buffer = NULL;  // global linked list of packet structs

// Insert a node at the head of a linked list:
int LIST_InsertHeadNode(LIST_NODE_T **IO_head, int id, int bytes, float atime, float dtime) {
	int rCode = 0;
	LIST_NODE_T *newNode = NULL;
	
	/* Allocate memory for new node (with its payload). */
	newNode = malloc(sizeof(*newNode));
	if (NULL == newNode) {
		rCode = ENOMEM;   /* ENOMEM is defined in errno.h */
		fprintf(stderr, "malloc() failed.\n");
		goto CLEANUP;
	}

	/* Initialize the new node's payload. */   
	newNode->packet.id		  	   = id;
	newNode->packet.num_bytes 	   = bytes;
	newNode->packet.arrival_time   = atime;
	newNode->packet.departure_time = dtime;

	/* Link this node into the list as the new head node. */
	newNode->next = *IO_head;
	*IO_head = newNode;

	CLEANUP:
	return rCode;
}

// Return a pointer to the tail node of a linked list:
int LIST_GetTailNode(LIST_NODE_T *I__listHead, LIST_NODE_T **_O_listTail) {
	int rCode = 0;
	LIST_NODE_T *curNode = I__listHead;

	/* Iterate through all list nodes until the last node is found. */
	/* The last node's 'next' field, which is always NULL. */
	if (curNode) {
		while (curNode->next)
			curNode = curNode->next;
	}

	/* Set the caller's pointer to point to the last (ie: tail) node. */
	if (_O_listTail)
		*_O_listTail = curNode;

	return rCode;
}

// Append a new node to the tail of a linked list:
int LIST_InsertTailNode(LIST_NODE_T **IO_head, int id, int bytes, float atime, float dtime) {
	int rCode = 0;
	LIST_NODE_T *tailNode;
	LIST_NODE_T *newNode = NULL;

	/* Get a pointer to the last node in the list. */
	rCode = LIST_GetTailNode(*IO_head, &tailNode);
	if (rCode)
		goto CLEANUP;
	
	/* Allocate memory for new node (with its payload). */
	newNode = malloc(sizeof(*newNode));
	if (NULL == newNode) {
		rCode = ENOMEM;   /* ENOMEM is defined in errno.h */
		fprintf(stderr, "malloc() failed.\n");
		goto CLEANUP;
	}

	/* Initialize the new node's payload. */   
	newNode->packet.id		  	   = id;
	newNode->packet.num_bytes 	   = bytes;
	newNode->packet.arrival_time   = atime;
	newNode->packet.departure_time = dtime;

	/* Link this node into the list as the new tail node. */
	newNode->next = NULL;
	if (tailNode)
		tailNode->next = newNode;
	else
		*IO_head = newNode;

	CLEANUP:
	return rCode;
}

// Return a pointer to a specific node based on packet ID:
int LIST_FetchNodeByID(LIST_NODE_T *I__head, int id, LIST_NODE_T **_O_node, LIST_NODE_T **_O_parent) {
	int rCode = 0;
	LIST_NODE_T *parent = NULL;
	LIST_NODE_T *curNode = I__head;

	// Search the list for a matching packet id:
	while (curNode) {
		if (curNode->packet.id == id)
			break;
		parent = curNode;  // Remember this node; it will be the parent of the next
		curNode = curNode->next;
	}

	// If no match is found, inform the caller:
	if (NULL == curNode) {
		rCode = ENOENT;
		goto CLEANUP;
	}

	// Return the matching node to the caller:
	if (_O_node)
		*_O_node = curNode;

	// Return parent node to the caller:
	if (_O_parent)
		*_O_parent = parent;

	CLEANUP:
	return rCode;
}

// Unlink & deallocate memory for a single node of a linked list:
int LIST_DeleteNode(LIST_NODE_T **IO_head, int id) {
	int rCode = 0;
	LIST_NODE_T *parent;
	LIST_NODE_T *delNode = NULL;

	/* Find the node to delete. */
	rCode = LIST_FetchNodeByID(*IO_head, id, &delNode, &parent); 
	switch (rCode) {
		case 0:
			break;
		case ENOENT:
			fprintf(stderr, "Matching node not found.\n");
			goto CLEANUP;
		default:
			fprintf(stderr, "LIST_FetchNodeByID() reports: %d\n", rCode);
			goto CLEANUP;
	}
	
	// Unlink node to delete then deallocate memory:
	if (NULL == parent)
		*IO_head = delNode->next;
	else
		parent->next = delNode->next;

	// Free the delNode and its packet:
	free(delNode);

	CLEANUP:
	return rCode;
}    

// Deallocate an entire linked list:
void LIST_FullDelete(LIST_NODE_T *head) {
	LIST_NODE_T *cur = head;
	
	if (NULL != cur) {
		if (NULL != cur->next) {
			LIST_FullDelete(cur->next);
		}
		free(cur);
	}
	return;
}

// Print simulation statistics:
void PrintSummary() {
	printf("Global time: %f, Buff: %d, Proc: %f\n", global_time, buff_ctr, processed_bytes);			
}

// Conditionally deliver packets out of buffer each simulation loop:		     
void ProcessPackets() {
	LIST_NODE_T *cur = buffer;
	NODE_PACKET_T p;
	
	// Iterate over all buffered packets:
	while (cur) {
		p = cur->packet;	
		cur = cur->next;
		
		// Deliver packet if enough processing time has elapsed:
		if (processed_bytes >= p.num_bytes) {
			
			// Update counters:
			processed_bytes -= p.num_bytes;
			buff_ctr -= 1;
			delivered_pkts += 1;
			
			// Use current packet's departure time
			// to update the total delay:
			p.departure_time = global_time;
			total_delay += p.departure_time - p.arrival_time;
			if (debug) {
				PrintSummary();
				printf("\tDELIVERED: ID: %d, A: %f, D: %f, Size: %d\n", p.id, p.arrival_time, p.departure_time, p.num_bytes);
			}
			
			// Remove packet from buffer & continue:
			LIST_DeleteNode(&buffer, p.id);
			continue;
		}
		else
			break;  // quit once no more packets can be delivered (in FIFO manner)
	}
	return;		
}

// Reset global vars between simulations:
void reset_globals(int i) {
	tr_out = outbound_rates[i];
	total_pkts = 0, delivered_pkts = 0, dropped_pkts = 0;
	buff_ctr = 0, buff_size = 100, id = 0;
	processed_bytes = 0.0, total_delay = 0.0;
	global_time = START_TIME;
	buffer = NULL;
	return;
}

// Parse arrival time & packet size from video trace into  a packet struct:
NODE_PACKET_T extract_data(char * line) {
	NODE_PACKET_T p;
	int i = 0;
	char *tok = strtok(line, " "); 
		
	// Extract timestamp and packet size:
	while (tok != NULL) {
		if (i == 0)
			p.arrival_time = atof(tok);
		if (i == 1)
			p.num_bytes = atoi(tok);
		i += 1;
		tok = strtok(NULL, " ");
	}
	
	p.departure_time = 0;
	return p;
}
	
// Compute QoS simulation for VIDEO_TRACE file and output results:
void run_qos_simulation() {
	
	// Open VIDEO_TRACE file:
	FILE * f = fopen(VIDEO_TRACE, "rb");
	if (f == NULL) {
		puts("Failed to open video trace.");
		return;
	}
	
	// Declare initial packet struct w/ read-next-line flag set:
	NODE_PACKET_T p;
	p.id = -1;		  // Read-next-line flag
	char * result;    // File I/O indicator
	int quit = 0;	  // End simulation flag
	
	// Execute simulation loop:
	while (1) {
		
		// Break once all packets have arrived + buffer is cleared:
		if (quit == 1 && buff_ctr == 0)
			break;
		
		// Parse video trace line into a packet struct
		// if the packet has arrived (enough time passed): 
		if (p.id == -1 && quit == 0) {
			result = fgets(line, sizeof(line), f);
			
			// If the last packet has arrived, cue simulation timeout:
			if (NULL == result)
				quit = 1;
	
			// Otherwise, instantiate the packet:
			else {
				p = extract_data(line);
				p.id = id;
				id += 1;
				total_pkts += 1;
			}
		}
		
		// Next packet has arrived:
		if (global_time >= p.arrival_time && p.id != -1) {
			
			// Drop the packet if buffer is full:
			if (buff_ctr == buff_size && p.id != -1) {
				dropped_pkts += 1;
				if (debug) {
					PrintSummary();
					printf("\tDROPPED:   ID: %d, A: %f, D: %f, Size: %d\n", p.id, p.arrival_time, p.departure_time, p.num_bytes);
				}
				p.id = -1;  // reset read-next-line flag
			}
			// Otherwise, create / append to buffer:
			else {
				if (buffer == NULL)
					LIST_InsertHeadNode(&buffer, p.id, p.num_bytes, p.arrival_time, p.departure_time);
				else
					LIST_InsertTailNode(&buffer, p.id, p.num_bytes, p.arrival_time, p.departure_time);
				
				// Increment buffer counter + reset read-next-line flag:
				if (debug) {
					PrintSummary();
					printf("\tBUFFERED:  ID: %d, A: %f, D: %f, Size: %d\n", p.id, p.arrival_time, p.departure_time, p.num_bytes);
				}
				buff_ctr += 1;
				p.id = -1;
			}
		}
		
		// Simulate outbound data stream:
		if (buff_ctr > 0)
			processed_bytes += TIME_UNIT * OUTGOING_BYTES_PER_SECOND;
		// Reset # processed bytes if buffer empty:
		else if (buff_ctr == 0)
			processed_bytes = 0;
		// Attempt to deliver packets out of buffer:
		ProcessPackets();
		
		// Increment global time counter:
		global_time += TIME_UNIT;
	}
	
	// Calculate packet loss percentage & average queueing delay:
	float avg_queueing_delay, packet_loss_pct;
	packet_loss_pct = ((float) dropped_pkts / (float) total_pkts) * 100;
	avg_queueing_delay = total_delay / total_pkts;
	
	// Output results:
	printf("%d \t\t%d \t\t%d \t\t%2.3f \t\t%.6f\n", total_pkts, delivered_pkts, dropped_pkts, packet_loss_pct, avg_queueing_delay);
	
	// Deallocate memory for buffer linked list:
	LIST_FullDelete(buffer);
	
	// Close video trace file and exit:
	fclose(f);
	return;
}

/* 
 * Either:
 * 
 * 1) Run QoS simulation for each outbound transmission rate
 *    and a fixed buffer size, B, equal to 100; or,
 *
 * 2) Run with a fixed transmission rate of 11Mbps and vary the buffer
 *    size from zero to n, where n is a command line argument.
 * 
 * Args:
 * 	n - maximum buffer size (optional)
 * 
 * Returns:
 *  QoS metrics for each simulation run in a tabular format.
 * 
 */
int main(int charc, char *argv[]) {
	int n;
	bool default_mode = charc == 1;
	
	// Print intro message + instantiate loop counter based on mode:
	if (default_mode) {
		n = 4;
		printf("\nOutputting summary statistics for outbound WiFi transmission rate = 11, 6, 30, 54...\n\n");
	}
	else {
		n = atoi(argv[1]);
		printf("\nOutputting summary statistics for outbound WiFi transmission rate = 11, \n");
		printf("while varying buffer size from 0 to %d ...\n\n", n);	
	}
	
	// Print output header:
	printf("########################################################################################\n");
	printf("Incoming Pkts \tDelivered Pkts \tLost Pkts \tPkt Loss %% \tAvg. Queuing Delay (sec)\n");
	printf("########################################################################################\n");
	 
	// Run QoS simulation n times, varying tr_out & buff_size based on mode:
	for (int i = 0; i < n; i++) {
		reset_globals((default_mode) ? i : 0);
		if (!default_mode) buff_size = i;
		run_qos_simulation();
	}
	
	// Print final row & exit:
	printf("########################################################################################\n\n");
	exit(0);
}
