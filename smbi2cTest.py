from smbus2 import SMBus
from time import sleep

# I2C channel 1 is connected to the GPIO pins
channel = 1

#  Arduino will be addr 0x10
address = 0x10

startMsg = 0x55
endMsg = 0xaa
fanRangeStart = 0
fanRangeEnd = 50 #full speed
# Initialize I2C (SMBus)
bus = SMBus(channel)

def sendSpeed(speed):
    # Create a message sending the desired fan speed for each fan as a byte
    msg = [startMsg, speed, speed, speed, speed, endMsg]

    # Write out I2C command: address, offset, msg
    try: 
        bus.write_i2c_block_data(address, 0, msg)
        print (f"Sent: {msg}")
    except OSError:
        print(f"Failed to send message {msg}")

while True:
    for speed in range(fanRangeStart, fanRangeEnd, 5) :
        sendSpeed(speed)
        sleep(2.0) # takes 20 seconds to ramp up
    for speed in range(fanRangeEnd, fanRangeStart, -5) :
        sendSpeed(speed)
        sleep(2.0) # takes 20 seconds to ramp up


