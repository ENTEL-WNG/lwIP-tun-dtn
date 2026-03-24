#!/bin/bash
set -e

echo "--- Setting interfaces and routes ---"

i=1
ALL_INTERFACES=()
while true; do
    VAR_NAME="INT_$i"
    TEMP_NAME="TEMP_$i"
    
    if [[ -n "${!VAR_NAME}" ]]; then
        IFS=', ' read -r -a ARRAY <<< "${!VAR_NAME}"
        length=${#ARRAY[@]}
        INT="${ARRAY[0]}"
        ADDR="${ARRAY[1]}"
        ALL_INTERFACES+=("$INT")

        echo "=== $i $INT ==="
        OLD_INT=$(ip -o link show | awk -F': ' '{print $2}' | grep '^eth' | sed 's/@.*//' | sed -n '1p')
        if [ -n "$OLD_INT" ]; then
            echo "Renaming Docker interfaces: $OLD_INT->$INT"
            echo "ip link set "$OLD_INT" down && ip link set "$OLD_INT" name $TEMP_NAME"
            echo "ip link set $TEMP_NAME name "$INT" && ip link set "$INT" up"
            ip link set "$OLD_INT" down && ip link set "$OLD_INT" name $TEMP_NAME
            ip link set $TEMP_NAME name "$INT" && ip link set "$INT" up
        else
            echo "Warning: Could not find two eth interfaces to rename. Using existing names."
        fi

        echo "ip -6 addr add $ADDR/64 dev $INT || true"
        ip -6 addr add $ADDR/64 dev $INT || true

        if [ "${#ARRAY[@]}" -gt 2 ]; then
            VIA_ADDR="${ARRAY[2]}"

            for (( j=3; j<${#ARRAY[@]}; j++ ));
            do
                ROUTE="${ARRAY[$j]}"

                echo "ip -6 route add $ROUTE/64 via $VIA_ADDR dev $INT || true"
                ip -6 route add $ROUTE/64 via $VIA_ADDR dev $INT || true
            done
        fi
    else
        echo "No interface for $VAR_NAME. Route setup complete"
        break
    fi
    
    ((i++))
done

echo "--- Seeding static NDP neighbor entries ---"
 
j=1
while true; do
    NEIGH_VAR="NEIGH_$j"
 
    if [[ -n "${!NEIGH_VAR}" ]]; then
        IFS=', ' read -r -a NEIGH <<< "${!NEIGH_VAR}"
        NEIGH_DEV="${NEIGH[0]}"
        NEIGH_IP="${NEIGH[1]}"
        NEIGH_MAC="${NEIGH[2]}"
 
        echo "ip -6 neigh replace $NEIGH_IP lladdr $NEIGH_MAC dev $NEIGH_DEV nud permanent"
        ip -6 neigh replace "$NEIGH_IP" lladdr "$NEIGH_MAC" dev "$NEIGH_DEV" nud permanent || true
    else
        echo "No neighbor for $NEIGH_VAR. NDP seeding complete."
        break
    fi
 
    ((j++))
done