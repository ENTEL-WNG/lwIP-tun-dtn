import sys
import time
import numpy as np
from skyfield.api import load, wgs84, Distance
from datetime import timedelta, datetime, timezone
from itertools import combinations
import os
from pathlib import Path

# Data rate of the links
RATE_MBPS = 300 # Mbps
# Minimum elevation over the horizon for a GS-SAT contact to happen
MIN_ELEVATION = 55 # Degrees


class Ns3TopologyGenerator:
    def __init__(self, tle_path, ground_stations, isl_max_range_km=5000):
        self.ts = load.timescale()
        self.isl_max_range_km = isl_max_range_km
        self.ground_nodes = {}
        # Load satellites
        self.satellites = load.tle_file(tle_path)
        self.node_registry = [] # List of names, index is the ID (0-based)

        # 1. Register Satellites (IDs 0 to M-1)
        for sat in self.satellites:
            self.node_registry.append(sat.name)

        # 2. Register Ground Stations (IDs M to N-1)
        for name, coords in ground_stations.items():
            self.ground_nodes[name] = wgs84.latlon(coords[0], coords[1], elevation_m=coords[2])
            self.node_registry.append(name)
            
        self.num_nodes = len(self.node_registry)
        self.speed_of_light_km_s = 299792.458

    def get_id(self, name):
        return self.node_registry.index(name)

    def generate(self, start_time, duration_hours, step_seconds=30, compute_isl=False):
        t_start = self.ts.from_datetime(start_time)
        t_end = self.ts.from_datetime(start_time + timedelta(hours=duration_hours))
        
        times = []
        curr = t_start
        while curr.tt <= t_end.tt:
            times.append(curr)
            curr = self.ts.from_datetime(curr.utc_datetime() + timedelta(seconds=step_seconds))

        n_steps = len(times)
        n_gs = len(self.ground_nodes)
        n_sat = len(self.satellites)
        n_isl_pairs = len(self.satellites) * (len(self.satellites) - 1) // 2 if compute_isl else 0
        print(f"Computing topology: {self.num_nodes} nodes, {n_steps} steps "
              f"({duration_hours}h @ {step_seconds}s)  "
              f"GS={n_gs}  SAT={n_sat}"
              + (f"  ISL pairs/step={n_isl_pairs}" if compute_isl else ""))

        report_every = max(1, n_steps // 20)   # ~5% intervals
        wall_start = time.monotonic()

        # Tracking active links to merge time steps
        # Value is now tuple: (start_time, list_of_distances)
        active_links = {}
        completed_contacts = []

        for i, t in enumerate(times):
            if i % report_every == 0:
                elapsed = time.monotonic() - wall_start
                eta = (elapsed / i * (n_steps - i)) if i > 0 else 0.0
                print(f"  {i / n_steps * 100:5.1f}%  "
                      f"step {i}/{n_steps}  "
                      f"elapsed {elapsed:6.1f}s  eta {eta:6.1f}s  "
                      f"contacts={len(completed_contacts)}",
                      flush=True)

            utc_t = t.utc_datetime()
            current_connectivity = {}  # Key: link_tuple, Value: distance_km

            # A. GS <-> SAT
            for gs_name, gs_obj in self.ground_nodes.items():
                for sat in self.satellites:
                    diff = sat - gs_obj
                    topocentric = diff.at(t)
                    alt, _, d = topocentric.altaz()
                    
                    if alt.degrees >= MIN_ELEVATION:
                        u, v = self.get_id(gs_name), self.get_id(sat.name)
                        link = tuple(sorted((u, v)))
                        current_connectivity[link] = d.km

            # B. SAT <-> SAT
            if compute_isl:
                # Get positions once per step
                sat_positions = {sat.name: sat.at(t).position.km for sat in self.satellites}
                
                for sat1, sat2 in combinations(self.satellites, 2):
                    p1 = sat_positions[sat1.name]
                    p2 = sat_positions[sat2.name]
                    dist_km = np.linalg.norm(p1 - p2)
                    
                    if dist_km <= self.isl_max_range_km:
                        u, v = self.get_id(sat1.name), self.get_id(sat2.name)
                        link = tuple(sorted((u, v)))
                        current_connectivity[link] = dist_km

            # C. Detect State Changes
            
            # 1. Check for Broken Links
            for link in list(active_links.keys()):
                if link not in current_connectivity:
                    # Link just ended
                    start_t, dist_samples = active_links[link]
                    avg_dist = sum(dist_samples) / len(dist_samples)
                    
                    completed_contacts.append({
                        "u": link[0], "v": link[1],
                        "start": start_t, "end": utc_t,
                        "distance_km": avg_dist
                    })
                    del active_links[link]
                else:
                    # Link still active, record new distance sample for averaging
                    active_links[link][1].append(current_connectivity[link])
            
            # 2. Check for New Links
            for link, dist_km in current_connectivity.items():
                if link not in active_links:
                    # Start new tracking: (start_time, [first_distance])
                    active_links[link] = (utc_t, [dist_km])

        # Close remaining open links at simulation end
        final_time = times[-1].utc_datetime()
        for link, (start_t, dist_samples) in active_links.items():
            avg_dist = sum(dist_samples) / len(dist_samples)
            completed_contacts.append({
                "u": link[0], "v": link[1],
                "start": start_t, "end": final_time,
                "distance_km": avg_dist
            })

        total_elapsed = time.monotonic() - wall_start
        print(f"  100.0%  done in {total_elapsed:.1f}s  "
              f"contacts={len(completed_contacts)}")

        return completed_contacts

    def export_ns3(self, contacts, start_time_ref, topology, output_dir="topologies"):
        os.makedirs(output_dir, exist_ok=True)
        
        # 1. Export Node Map
        with open(f"{output_dir}/{topology}_nodes.txt", "w") as f:
            f.write(f"# Total Nodes: {self.num_nodes}\n")
            for idx, name in enumerate(self.node_registry):
                type_str = "GS" if name in self.ground_nodes else "SAT"
                f.write(f"{idx} {name} {type_str}\n")

        # 2. Export Contacts
        ref_ts = start_time_ref.timestamp()
        
        with open(f"{output_dir}/{topology}_contacts.dat", "w") as f:
            for c in contacts:
                start_rel = max(0, c["start"].timestamp() - ref_ts)
                end_rel = max(0, c["end"].timestamp() - ref_ts)
                if end_rel <= start_rel: continue
                
                # Calculate One-Way Light Time (Delay) in milliseconds
                # Delay = (Distance km / Speed of Light km/s) * 1000
                delay_ms = (c["distance_km"] / self.speed_of_light_km_s) * 1000.0
                
                # Enforce a minimum physical processing delay if needed (e.g. +1ms)
                final_delay = max(0.1, delay_ms) 

                f.write(f"{c['u']} {c['v']} {start_rel:.3f} {end_rel:.3f} {RATE_MBPS}Mbps {final_delay:.3f}ms\n")
        
        print(f"Exported {len(contacts)} contacts to {output_dir}/{topology}_contacts.dat")

    def export_contact_plan(self, contacts, start_time_ref, topology, output_dir="topologies", bidirected=True):
        os.makedirs(output_dir, exist_ok=True)

        name = f"contact_plan_{topology}"
        ref_ts = start_time_ref.timestamp()

        valid = []
        for c in contacts:
            start_rel = max(0.0, c["start"].timestamp() - ref_ts)
            end_rel   = max(0.0, c["end"].timestamp()   - ref_ts)
            if end_rel > start_rel:
                valid.append({**c, "start_rel": start_rel, "end_rel": end_rel})

        max_time = int(max((c["end_rel"] for c in valid), default=0))
        node_ids = sorted({nid for c in valid for nid in (c["u"], c["v"])})
        rate_bps = RATE_MBPS * 1_000_000

        lines = []
        lines.append("[contact_plan]")
        lines.append(f'name                 = "{name}"')
        lines.append(f"max_time_in_sec      = {max_time}")
        lines.append("")
        lines.append("[contact_plan.defaults]")
        lines.append(f"rate_in_bits_per_sec = {rate_bps}")
        lines.append(f"range                = 1")
        lines.append("")
        lines.append("# Node Definitions")
        id_map = {nid: i + 1 for i, nid in enumerate(node_ids)}
        for nid in node_ids:
            node_name = self.node_registry[nid]
            is_dtn = node_name not in self.ground_nodes
            lines.append("[[nodes]]")
            lines.append(f"id        = {id_map[nid]}")
            lines.append(f'name      = "{node_name}"')
            lines.append(f"isDtnNode = {str(is_dtn).lower()}")
            lines.append("")
        lines.append("# Edge Definitions")
        for c in valid:
            range_s = c["distance_km"] / self.speed_of_light_km_s
            lines.append("[[edges]]")
            lines.append(f"from         = {id_map[c['u']]}")
            lines.append(f"to           = {id_map[c['v']]}")
            lines.append(f"start_in_sec = {int(c['start_rel'])}")
            lines.append(f"end_in_sec   = {int(c['end_rel'])}")
            lines.append(f"bidirected   = {str(bidirected).lower()}")
            lines.append(f"range        = {range_s:.6f}")
            lines.append("")

        output_path = os.path.join(output_dir, f"contact-plan-{topology}.toml")
        with open(output_path, "w") as f:
            f.write("\n".join(lines) + "\n")

        print(f"Exported contact plan '{name}' with {len(node_ids)} nodes and {len(valid)} edges to {output_path}")

def dms_to_dd(lat_deg, lat_min, lat_sec, lat_dir, lon_deg, lon_min, lon_sec, lon_dir, height=0):
    lat = lat_deg + lat_min/60 + lat_sec/3600
    if lat_dir == 'S':
        lat = -lat
    lon = lon_deg + lon_min/60 + lon_sec/3600
    if lon_dir == 'W':
        lon = -lon
    return lat, lon, height

if __name__ == "__main__":
    topology_path = sys.argv[1] if len(sys.argv) > 1 else "topologies/sateliot.tle"

    # stations = {"GS_Barcelona": (41.4, 2.1, 0), "GS_Tokyo": (35.6, 139.6, 0)}
    # Antarticaa: 74°31'21.4"S 73°47'56.9"W 
    # Svalbard: 79°11'55.1"N 11°59'16.1"E
    antarctica = dms_to_dd(74, 31, 21.4, 'S', 73, 47, 56.9, 'W')
    svalbard = dms_to_dd(79, 11, 55.1, 'N', 11, 59, 16.1, 'E')
    stations = {"GS_Antarctica": antarctica, "GS_SVALBARD": svalbard}

    topology_name = name = Path(topology_path).stem
    print(topology_path)
    gen = Ns3TopologyGenerator(topology_path, stations)
    
    start = datetime(2026, 6, 19, 12, 0, 0, tzinfo=timezone.utc)
    contacts = gen.generate(start, duration_hours=25, step_seconds=10, compute_isl=True)
    gen.export_ns3(contacts, start, topology_name)
    gen.export_contact_plan(contacts, start, topology_name)
