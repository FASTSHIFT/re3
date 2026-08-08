#!/bin/bash
cd /home/pi/GTA3
# Ensure log file is owned by pi before redirecting.
# sudo calls below can cause the shell to create it as root if we
# remove-then-recreate lazily (observed after reboot with OC applied).
rm -f /home/pi/re3spi.log
touch /home/pi/re3spi.log
echo pi | sudo -S systemctl stop lightdm 2>/dev/null; sleep 2
# Set headphone volume (GamePi20 GPIO18/19 PWM audio)
amixer -c 1 sset PCM 80% 2>/dev/null
echo pi | sudo -S rm -f /dev/shm/sem.semaphore_* 2>/dev/null
setsid env DISPLAY= RE3_HUD=1 RE3_TIME_PRESENT=1 \
  ./re3 >> /home/pi/re3spi.log 2>&1 < /dev/null &
echo "launched pid $!"
