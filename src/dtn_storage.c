// dtn_storage.c: Implementation of persistent packet storage with disk-based backup for DTN store-and-forward functionality
// Copyright (C) 2025 Michael Karpov
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "dtn_storage.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/ip6_addr.h"
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include "dtn_custody.h"

// File header for stored packets
typedef struct {
    char magic[4];             // DTN Packet
    u32_t version;             // File format version
    u32_t timestamp;           // When the packet was stored
    u32_t packet_len;          // Length of the packet data
    ip6_addr_t original_dest;  // Original destination
} PacketFileHeader;

// Creates storage directory if it doesn't exist
int dtn_storage_init_directory(Storage_Function* storage) {
    struct stat st = {0};
    
    if (stat(storage->storage_directory, &st) == -1) {
        printf("DTN Storage: Creating directory %s\n", storage->storage_directory);
        int result = mkdir(storage->storage_directory, 0755);
        if (result == -1) {
            perror("DTN Storage: Failed to create storage directory");
            return 0;
        }
    }
    return 1;
}

int dtn_storage_save_packet_to_disk(Storage_Function* storage, Stored_Packet_Entry* entry) {
    if (!storage || !entry || !entry->p) return 0;
    
    char addr_str[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(&entry->original_dest, addr_str, sizeof(addr_str));
    
    for (char* p = addr_str; *p; p++) {
        if (*p == ':') *p = '_';
    }
    
    if (snprintf(entry->filename, MAX_PATH_LENGTH, "%s/pkt_%s_%u.dat", 
                storage->storage_directory, addr_str, entry->stored_time_ms) >= MAX_PATH_LENGTH) {
        fprintf(stderr, "DTN Storage: Warning - Filename was truncated\n");
    }
    
    FILE* file = fopen(entry->filename, "wb");
    if (!file) {
        perror("DTN Storage: Failed to open file for writing");
        return 0;
    }
    
    PacketFileHeader header;
    memcpy(header.magic, "DTNP", 4);
    header.version = 1;
    header.timestamp = entry->stored_time_ms;
    header.packet_len = entry->p->tot_len;
    memcpy(&header.original_dest, &entry->original_dest, sizeof(ip6_addr_t));
    
    if (fwrite(&header, sizeof(header), 1, file) != 1) {
        perror("DTN Storage: Failed to write packet header");
        fclose(file);
        return 0;
    }
    
    char* buffer = malloc(entry->p->tot_len);
    if (!buffer) {
        perror("DTN Storage: Failed to allocate buffer for packet data");
        fclose(file);
        return 0;
    }
    
    if (pbuf_copy_partial(entry->p, buffer, entry->p->tot_len, 0) != entry->p->tot_len) {
        perror("DTN Storage: Failed to copy packet data to buffer");
        free(buffer);
        fclose(file);
        return 0;
    }
    
    if (fwrite(buffer, 1, entry->p->tot_len, file) != entry->p->tot_len) {
        perror("DTN Storage: Failed to write packet data");
        free(buffer);
        fclose(file);
        return 0;
    }
    
    free(buffer);
    fclose(file);
    
    printf("DTN Storage: Packet saved to %s\n", entry->filename);
    return 1;
}

int dtn_storage_remove_packet_from_disk(Storage_Function* storage, const char* filename) {
    if (!storage || !filename) return 0;
    
    if (remove(filename) != 0) {
        perror("DTN Storage: Failed to remove packet file");
        return 0;
    }
    
    printf("DTN Storage: Removed packet file %s\n", filename);
    return 1;
}

static Stored_Packet_Entry* dtn_storage_load_packet_from_file(Storage_Function* storage, const char* filename) {
    if (!storage || !filename) return NULL;
    
    FILE* file = fopen(filename, "rb");
    if (!file) {
        perror("DTN Storage: Failed to open packet file for reading");
        return NULL;
    }
    
    PacketFileHeader header;
    if (fread(&header, sizeof(header), 1, file) != 1) {
        perror("DTN Storage: Failed to read packet header");
        fclose(file);
        return NULL;
    }
    
    if (memcmp(header.magic, "DTNP", 4) != 0) {
        fprintf(stderr, "DTN Storage: Invalid packet file format\n");
        fclose(file);
        return NULL;
    }
    
    char* buffer = malloc(header.packet_len);
    if (!buffer) {
        perror("DTN Storage: Failed to allocate buffer for packet data");
        fclose(file);
        return NULL;
    }
    
    if (fread(buffer, 1, header.packet_len, file) != header.packet_len) {
        perror("DTN Storage: Failed to read packet data");
        free(buffer);
        fclose(file);
        return NULL;
    }
    
    fclose(file);
    
    struct pbuf* p = pbuf_alloc(PBUF_RAW, header.packet_len, PBUF_RAM);
    if (!p) {
        perror("DTN Storage: Failed to allocate pbuf for loaded packet");
        free(buffer);
        return NULL;
    }
    
    if (pbuf_take(p, buffer, header.packet_len) != ERR_OK) {
        perror("DTN Storage: Failed to copy data to pbuf");
        pbuf_free(p);
        free(buffer);
        return NULL;
    }
    
    free(buffer);
    
    Stored_Packet_Entry* entry = malloc(sizeof(Stored_Packet_Entry));
    if (!entry) {
        perror("DTN Storage: Failed to allocate memory for packet entry");
        pbuf_free(p);
        return NULL;
    }
    
    entry->p = p;
    memcpy(&entry->original_dest, &header.original_dest, sizeof(ip6_addr_t));
    entry->stored_time_ms = header.timestamp;
    entry->next = NULL;
    
    strncpy(entry->filename, filename, MAX_PATH_LENGTH-1);
    entry->filename[MAX_PATH_LENGTH-1] = '\0';
    
    char addr_str[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(&entry->original_dest, addr_str, sizeof(addr_str));
    printf("DTN Storage: Loaded packet for %s from %s\n", addr_str, filename);
    
    return entry;
}

int dtn_storage_load_packets_from_disk(Storage_Function* storage) {
    if (!storage) return 0;
    
    DIR* dir = opendir(storage->storage_directory);
    if (!dir) {
        perror("DTN Storage: Failed to open storage directory");
        return 0;
    }
    
    int loaded_count = 0;
    struct dirent* entry;
    
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char* ext = strrchr(entry->d_name, '.');
        if (!ext || strcmp(ext, ".dat") != 0) {
            continue;
        }
        
        char full_path[PATH_MAX]; 
        
        if (strlen(storage->storage_directory) + strlen(entry->d_name) + 2 > sizeof(full_path)) {
            fprintf(stderr, "DTN Storage: Path too long for file %s, skipping\n", entry->d_name);
            continue;
        }
        
        strcpy(full_path, storage->storage_directory);
        strcat(full_path, "/");
        strcat(full_path, entry->d_name);
        
        Stored_Packet_Entry* packet_entry = dtn_storage_load_packet_from_file(storage, full_path);
        if (packet_entry) {
            if (storage->packet_list_head == NULL) {
                storage->packet_list_head = packet_entry;
            } else {
                Stored_Packet_Entry* current = storage->packet_list_head;
                while (current->next != NULL) {
                    current = current->next;
                }
                current->next = packet_entry;
            }
            
            storage->stored_packets_count++;
            loaded_count++;
            
            if (storage->stored_packets_count >= MAX_STORED_PACKETS) {
                printf("DTN Storage: Maximum packet count reached, stopping load\n");
                break;
            }
        }
    }
    
    closedir(dir);
    printf("DTN Storage: Loaded %d packets from disk\n", loaded_count);
    return loaded_count;
}

Storage_Function* dtn_storage_create(DTN_Module* parent) {
    Storage_Function* storage = (Storage_Function*)malloc(sizeof(Storage_Function));
    if (storage) {
        storage->parent_module = parent;
        storage->stored_packets_count = 0;
        storage->max_storage_bytes = 1024 * 1024; // 1MB limit
        storage->packet_list_head = NULL;
        
        strncpy(storage->storage_directory, STORAGE_DIR, MAX_PATH_LENGTH - 1);
        storage->storage_directory[MAX_PATH_LENGTH - 1] = '\0';
        
        printf("DTN Storage Function created (Max: %zu bytes, Max Packets: %d).\n", 
               storage->max_storage_bytes, MAX_STORED_PACKETS);
        
        if (!dtn_storage_init_directory(storage)) {
            fprintf(stderr, "DTN Storage: Failed to initialize storage directory\n");
            free(storage);
            return NULL;
        }
        
        dtn_storage_load_packets_from_disk(storage);
    } else {
        perror("Failed to allocate memory for Storage_Function");
    }
    return storage;
}

void dtn_storage_destroy(Storage_Function* storage) {
    if (!storage) return;
    printf("Destroying DTN Storage Function...\n");

    Stored_Packet_Entry* current = storage->packet_list_head;
    Stored_Packet_Entry* next_entry;
    while (current != NULL) {
        next_entry = current->next;
        char addr_str[IP6ADDR_STRLEN_MAX];
        ip6addr_ntoa_r(&current->original_dest, addr_str, sizeof(addr_str));
        printf("DTN Storage: Freeing stored pbuf (original dest: %s) during destroy.\n", addr_str);
        pbuf_free(current->p); 
        free(current);         
        current = next_entry;
    }
    storage->packet_list_head = NULL;
    storage->stored_packets_count = 0;

    free(storage);
}

int dtn_storage_is_full(Storage_Function* storage) {
    if (!storage) return 1;
    return storage->stored_packets_count >= MAX_STORED_PACKETS;
}

int dtn_storage_store_packet(Storage_Function* storage, struct pbuf* p, const ip6_addr_t* original_dest) {
    if (!storage || !p || !original_dest) {
        fprintf(stderr, "DTN Storage: Invalid arguments to store_packet.\n");
        return 0;
    }

    if (dtn_storage_is_full(storage)) {
        char addr_str[IP6ADDR_STRLEN_MAX];
        ip6addr_ntoa_r(original_dest, addr_str, sizeof(addr_str));
        printf("DTN Storage: Storage is full. Cannot store packet for %s.\n", addr_str);
        return 0;
    }
    
    // Create a copy of the packet to strip headers if needed
    struct pbuf *p_to_store = pbuf_alloc(PBUF_RAW, p->tot_len, PBUF_RAM);
    if (!p_to_store) {
        perror("DTN Storage: Failed to allocate pbuf for storage copy");
        return 0;
    }
    
    if (pbuf_copy(p_to_store, p) != ERR_OK) {
        perror("DTN Storage: Failed to copy packet for storage");
        pbuf_free(p_to_store);
        return 0;
    }
    
    // Strip hop-by-hop header if present
    dtn_strip_custodian_option(&p_to_store);

    Stored_Packet_Entry* new_entry = (Stored_Packet_Entry*)malloc(sizeof(Stored_Packet_Entry));
    if (!new_entry) {
        perror("DTN Storage: Failed to allocate memory for Stored_Packet_Entry");
        pbuf_free(p_to_store);
        return 0;
    }

    new_entry->p = p_to_store; // Store the stripped version
    memcpy(&new_entry->original_dest, original_dest, sizeof(ip6_addr_t));
    new_entry->stored_time_ms = sys_now();
    new_entry->next = NULL;
    new_entry->filename[0] = '\0';
    
    if (!dtn_storage_save_packet_to_disk(storage, new_entry)) {
        fprintf(stderr, "DTN Storage: Failed to save packet to disk\n");
        pbuf_free(new_entry->p);
        free(new_entry);
        return 0;
    }

    if (storage->packet_list_head == NULL) {
        storage->packet_list_head = new_entry;
    } else {
        Stored_Packet_Entry* current_item = storage->packet_list_head;
        while (current_item->next != NULL) {
            current_item = current_item->next;
        }
        current_item->next = new_entry;
    }

    storage->stored_packets_count++;
    char addr_str_log[IP6ADDR_STRLEN_MAX];
    ip6addr_ntoa_r(original_dest, addr_str_log, sizeof(addr_str_log));
    printf("DTN Storage: Packet for %s stored successfully at time %u. Total stored: %zu\n",
           addr_str_log, new_entry->stored_time_ms, storage->stored_packets_count);

    return 1;
}

Stored_Packet_Entry* dtn_storage_retrieve_packet_for_dest(Storage_Function* storage, const ip6_addr_t* target_dest) {
    if (!storage || !target_dest || storage->packet_list_head == NULL) {
        return NULL;
    }

    Stored_Packet_Entry* current = storage->packet_list_head;
    Stored_Packet_Entry* prev = NULL;
    Stored_Packet_Entry* match = NULL;
    Stored_Packet_Entry* prev_for_match = NULL;

    current = storage->packet_list_head;
    prev = NULL;
    while(current != NULL) {
        ip6_addr_t current_dest_nozone;
        ip6_addr_t target_dest_nozone;

        memcpy(&current_dest_nozone, &current->original_dest, sizeof(ip6_addr_t));
        memcpy(&target_dest_nozone, target_dest, sizeof(ip6_addr_t));

#if LWIP_IPV6_SCOPES
        ip6_addr_set_zone(&current_dest_nozone, IP6_NO_ZONE);
        ip6_addr_set_zone(&target_dest_nozone, IP6_NO_ZONE);
#endif

        if (ip6_addr_cmp(&current_dest_nozone, &target_dest_nozone)) {
            match = current;
            prev_for_match = prev;
            break; 
        }
        prev = current;
        current = current->next;
    }

    if (match) {
        if (prev_for_match == NULL) {
            storage->packet_list_head = match->next;
        } else {
            prev_for_match->next = match->next;
        }
        storage->stored_packets_count--;
        
        char addr_str[IP6ADDR_STRLEN_MAX];
        ip6addr_ntoa_r(&match->original_dest, addr_str, sizeof(addr_str));
        printf("DTN Storage: Retrieving packet for %s (stored at %u). Total stored now: %zu\n",
               addr_str, match->stored_time_ms, storage->stored_packets_count);
        
        dtn_storage_remove_packet_from_disk(storage, match->filename);
        
        match->next = NULL;
        return match;
    }
    return NULL;
}

void dtn_storage_free_retrieved_entry_struct(Stored_Packet_Entry* entry) {
    if (entry) {
        char addr_str[IP6ADDR_STRLEN_MAX];
        ip6addr_ntoa_r(&entry->original_dest, addr_str, sizeof(addr_str));
        printf("DTN Storage: Freeing Stored_Packet_Entry structure for %s (pbuf management is caller's responsibility).\n", addr_str);
        free(entry);
    }
}

Stored_Packet_Entry* dtn_storage_get_packet_copy_for_dest(Storage_Function* storage, const ip6_addr_t* target_dest) {
    if (!storage || !target_dest || storage->packet_list_head == NULL) {
        return NULL;
    }

    Stored_Packet_Entry* current = storage->packet_list_head;
    
    while(current != NULL) {
        ip6_addr_t current_dest_nozone;
        ip6_addr_t target_dest_nozone;

        memcpy(&current_dest_nozone, &current->original_dest, sizeof(ip6_addr_t));
        memcpy(&target_dest_nozone, target_dest, sizeof(ip6_addr_t));

#if LWIP_IPV6_SCOPES
        ip6_addr_set_zone(&current_dest_nozone, IP6_NO_ZONE);
        ip6_addr_set_zone(&target_dest_nozone, IP6_NO_ZONE);
#endif

        if (ip6_addr_cmp(&current_dest_nozone, &target_dest_nozone)) {
            // Found a match, create a new entry
            Stored_Packet_Entry* copy = (Stored_Packet_Entry*)malloc(sizeof(Stored_Packet_Entry));
            if (!copy) {
                printf("DTN Storage: Failed to allocate memory for packet copy\n");
                return NULL;
            }
            
            // Copy the packet itself
            struct pbuf* p_copy = pbuf_alloc(PBUF_RAW, current->p->tot_len, PBUF_RAM);
            if (!p_copy) {
                printf("DTN Storage: Failed to allocate pbuf for packet copy\n");
                free(copy);
                return NULL;
            }
            
            if (pbuf_copy(p_copy, current->p) != ERR_OK) {
                printf("DTN Storage: Failed to copy packet data\n");
                pbuf_free(p_copy);
                free(copy);
                return NULL;
            }
            
            copy->p = p_copy;
            memcpy(&copy->original_dest, &current->original_dest, sizeof(ip6_addr_t));
            copy->stored_time_ms = current->stored_time_ms;
            copy->next = NULL;
            strncpy(copy->filename, current->filename, MAX_PATH_LENGTH-1);
            copy->filename[MAX_PATH_LENGTH-1] = '\0';
            
            char addr_str[IP6ADDR_STRLEN_MAX];
            ip6addr_ntoa_r(&copy->original_dest, addr_str, sizeof(addr_str));
            printf("DTN Storage: Created copy of packet for %s (original stored at %u)\n",
                   addr_str, copy->stored_time_ms);
            
            return copy;
        }
        
        current = current->next;
    }
    
    return NULL; 
}

void dtn_storage_delete_packet_by_ip_header(Storage_Function* storage, struct ip6_hdr* orig_ip6hdr) {
    if (!storage || !orig_ip6hdr || !storage->packet_list_head) {
        return;
    }
    
    ip6_addr_t orig_src, orig_dest;
    
    IP6_ADDR(&orig_src, 
             orig_ip6hdr->src.addr[0], 
             orig_ip6hdr->src.addr[1], 
             orig_ip6hdr->src.addr[2], 
             orig_ip6hdr->src.addr[3]);
             
    IP6_ADDR(&orig_dest, 
             orig_ip6hdr->dest.addr[0], 
             orig_ip6hdr->dest.addr[1], 
             orig_ip6hdr->dest.addr[2], 
             orig_ip6hdr->dest.addr[3]);
    
    char orig_src_str[IP6ADDR_STRLEN_MAX] = {0};
    char orig_dest_str[IP6ADDR_STRLEN_MAX] = {0};
    ip6addr_ntoa_r(&orig_src, orig_src_str, sizeof(orig_src_str));
    ip6addr_ntoa_r(&orig_dest, orig_dest_str, sizeof(orig_dest_str));
    
    printf("DTN Storage: Looking for stored packet matching src=%s, dest=%s\n", 
           orig_src_str, orig_dest_str);
    
    Stored_Packet_Entry* current = storage->packet_list_head;
    Stored_Packet_Entry* prev = NULL;
    bool found = false;
    
    while (current != NULL) {
        if (current->p && current->p->len >= IP6_HLEN) {
            struct ip6_hdr* stored_ip6hdr = (struct ip6_hdr*)current->p->payload;

            if (memcmp(&stored_ip6hdr->src,  &orig_ip6hdr->src,  16) == 0 &&
                memcmp(&stored_ip6hdr->dest, &orig_ip6hdr->dest, 16) == 0) {
 
                found = true;
     
                if (prev == NULL) {
                    storage->packet_list_head = current->next;
                } else {
                    prev->next = current->next;
                }
                
                printf("DTN Storage: Deleting stored packet for %s (src=%s) as next hop confirmed reception\n", 
                       orig_dest_str, orig_src_str);
                
                dtn_storage_remove_packet_from_disk(storage, current->filename);
                
                pbuf_free(current->p);
                free(current);
                
                storage->stored_packets_count--;
                
                break;
            }
        }
        
        prev = current;
        current = current->next;
    }
    
    if (!found) {
        printf("DTN Storage: No matching stored packet found for %s (src=%s)\n", 
               orig_dest_str, orig_src_str);
    }
}

void dtn_storage_delete_packet_by_icmp_data(Storage_Function* storage, struct pbuf* icmp_packet) {
    if (!storage || !icmp_packet || !storage->packet_list_head) {
        return;
    }
    
    // Parse ICMP packet structure:
    // IPv6 Header (40 bytes) + ICMPv6 Header (8 bytes) + Payload + Original IPv6 Header + 8 bytes of original payload
    
    if (icmp_packet->len < IP6_HLEN + 8 + 9 + IP6_HLEN + 8) {
        printf("DTN Storage: ICMP packet too small for proper parsing\n");
        return;
    }
    
    u8_t* icmp_data = (u8_t*)icmp_packet->payload + IP6_HLEN + 8; // Skip outer IPv6 + ICMP headers
    
    // Skip DTN payload structure (9 bytes: 4+2+2+1)
    icmp_data += 9; 
    
    // Now icmp_data points to the original IPv6 header
    struct ip6_hdr* orig_ip6hdr = (struct ip6_hdr*)icmp_data;
    u8_t* orig_first_8_bytes = icmp_data + IP6_HLEN; // First 8 bytes after IPv6 header
    
    ip6_addr_t orig_src, orig_dest;
    IP6_ADDR(&orig_src, 
             orig_ip6hdr->src.addr[0], orig_ip6hdr->src.addr[1], 
             orig_ip6hdr->src.addr[2], orig_ip6hdr->src.addr[3]);
    IP6_ADDR(&orig_dest, 
             orig_ip6hdr->dest.addr[0], orig_ip6hdr->dest.addr[1], 
             orig_ip6hdr->dest.addr[2], orig_ip6hdr->dest.addr[3]);
    
    char orig_src_str[IP6ADDR_STRLEN_MAX] = {0};
    char orig_dest_str[IP6ADDR_STRLEN_MAX] = {0};
    ip6addr_ntoa_r(&orig_src, orig_src_str, sizeof(orig_src_str));
    ip6addr_ntoa_r(&orig_dest, orig_dest_str, sizeof(orig_dest_str));
    
    printf("DTN Storage: Looking for stored packet matching src=%s, dest=%s with payload verification\n", 
           orig_src_str, orig_dest_str);
    
    // Parse the original packet to skip hop-by-hop headers in the ICMP data
    u8_t orig_nexth = orig_ip6hdr->_nexth;
    u16_t orig_payload_offset = 0;
    
    if (orig_nexth == IP6_NEXTH_HOPBYHOP) {
        // Skip hop-by-hop header to get to actual payload
        u8_t hbh_hdr_len = orig_first_8_bytes[1]; 
        orig_payload_offset = (hbh_hdr_len + 1) * 8; 
        orig_nexth = orig_first_8_bytes[0]; 
        
        printf("DTN Storage: Skipping hop-by-hop header (%d bytes) in ICMP data\n", orig_payload_offset);
    }
    
    // Get pointer to actual payload (after any headers)
    u8_t* orig_payload_start = orig_first_8_bytes + orig_payload_offset;
    u8_t actual_payload_bytes = 8 - orig_payload_offset;
    
    if (actual_payload_bytes <= 0) {
        printf("DTN Storage: Not enough payload data after headers, falling back to basic matching\n");
        dtn_storage_delete_packet_by_ip_header(storage, orig_ip6hdr);
        return;
    }
    
    Stored_Packet_Entry* current = storage->packet_list_head;
    Stored_Packet_Entry* prev = NULL;
    bool found = false;
    
    // Iterate through stored packets
    while (current != NULL) {
        if (current->p && current->p->len >= IP6_HLEN) {
            struct ip6_hdr* stored_ip6hdr = (struct ip6_hdr*)current->p->payload;
            
            // 1. Check src/dst addresses
            if (memcmp(&stored_ip6hdr->src, &orig_ip6hdr->src, 16) == 0) {
                
                // 2. Check next header protocol
                u8_t stored_nexth = stored_ip6hdr->_nexth;
                u16_t stored_payload_offset = IP6_HLEN;
                
                // Stored packets shouldn't have hop-by-hop headers, but check anyway
                if (stored_nexth == IP6_NEXTH_HOPBYHOP) {
                    printf("DTN Storage: Warning - stored packet has hop-by-hop header (unexpected)\n");
                    continue; 
                }
                
                // 3. Compare actual payload bytes
                bool payload_matches = true;
                if (actual_payload_bytes > 0 && current->p->len >= stored_payload_offset + actual_payload_bytes) {
                    u8_t* stored_payload = (u8_t*)current->p->payload + stored_payload_offset;
                    
                    if (memcmp(stored_payload, orig_payload_start, actual_payload_bytes) != 0) {
                        printf("DTN Storage: Payload mismatch for src=%s, dest=%s\n", orig_src_str, orig_dest_str);
                        payload_matches = false;
                    }
                }
                
                // 4. Check if protocols match (after stripping HBH from original)
                if (stored_nexth != orig_nexth) {
                    printf("DTN Storage: Protocol mismatch: stored=%d, orig=%d\n", stored_nexth, orig_nexth);
                    payload_matches = false;
                }
                
                if (payload_matches) {
                    found = true;
                    
                    // Remove from list
                    if (prev == NULL) {
                        storage->packet_list_head = current->next;
                    } else {
                        prev->next = current->next;
                    }
                    
                    printf("DTN Storage: Deleting stored packet for %s (src=%s) with payload verification\n", 
                           orig_dest_str, orig_src_str);
                    
                    // Remove from disk
                    dtn_storage_remove_packet_from_disk(storage, current->filename);
                    
                    pbuf_free(current->p);
                    free(current);
                    
                    storage->stored_packets_count--;
                    
                    break;
                }
            }
        }
        
        prev = current;
        current = current->next;
    }
    
    if (!found) {
        printf("DTN Storage: No matching stored packet found for %s (src=%s) with payload verification\n", 
               orig_dest_str, orig_src_str);
    }
}