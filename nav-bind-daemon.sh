#!/usr/bin/env bash

PHONE_MAC="2C:BE:EB:7F:2D:FA"
TARGET_SERVICE_NAME="NavJsonService"
LOG_TAG="[nav-bind]"

while true; do
  echo "$LOG_TAG checking RFCOMM binding..."

  # Récupère éventuellement la ligne rfcomm1 existante
  CURRENT_LINE=$(rfcomm | awk -v mac="$PHONE_MAC" '$1=="rfcomm1:" && $2==mac {print $0}')

  # Interroge SDP pour trouver le bon channel
  echo "$LOG_TAG scanning services on $PHONE_MAC ..."
  SDP_OUTPUT=$(sdptool browse "$PHONE_MAC" 2>/dev/null || true)

  if [ -z "$SDP_OUTPUT" ]; then
    echo "$LOG_TAG sdptool returned nothing (phone not reachable?), retry in 5s"
    sleep 5
    continue
  fi

  TARGET_CHANNEL=$(printf '%s\n' "$SDP_OUTPUT" \
    | awk -v name="$TARGET_SERVICE_NAME" '
        $0 ~ "Service Name: "name { in_block=1 }
        in_block && /Channel:/ { print $2; exit }
      ')

  if [ -z "$TARGET_CHANNEL" ]; then
    echo "$LOG_TAG cannot find service \"$TARGET_SERVICE_NAME\", retry in 5s"
    sleep 5
    continue
  fi

  echo "$LOG_TAG target RFCOMM channel $TARGET_CHANNEL"

  REBIND_NEEDED=1

  if [ -n "$CURRENT_LINE" ]; then
    # Exemple de CURRENT_LINE :
    # rfcomm1: 2C:BE:EB:7F:2D:FA channel 7 config
    CURRENT_CHANNEL=$(echo "$CURRENT_LINE" | awk '{for (i=1;i<=NF;i++) if ($i=="channel") print $(i+1)}')
    if echo "$CURRENT_LINE" | grep -q "closed"; then
      echo "$LOG_TAG rfcomm1 is CLOSED (channel $CURRENT_CHANNEL), will rebind"
      REBIND_NEEDED=1
    elif [ "$CURRENT_CHANNEL" != "$TARGET_CHANNEL" ]; then
      echo "$LOG_TAG rfcomm1 has wrong channel $CURRENT_CHANNEL, need $TARGET_CHANNEL, will rebind"
      REBIND_NEEDED=1
    else
      echo "$LOG_TAG rfcomm1 already bound on correct channel $CURRENT_CHANNEL, sleeping..."
      REBIND_NEEDED=0
    fi
  fi

  if [ "$REBIND_NEEDED" -ne 0 ]; then
    echo "$LOG_TAG rebinding rfcomm1 to channel $TARGET_CHANNEL"
    rfcomm release 1 2>/dev/null || true
    if rfcomm bind 1 "$PHONE_MAC" "$TARGET_CHANNEL"; then
      echo "$LOG_TAG bound /dev/rfcomm1 to $PHONE_MAC channel $TARGET_CHANNEL"
    else
      echo "$LOG_TAG rfcomm bind failed, retry in 5s"
    fi
  fi

  sleep 5
done