from smbus2 import SMBus
from time import sleep
import configparser
from fabric2 import Connection, ThreadingGroup, Result, exceptions
from paramiko import ssh_exception
from parse import parse, compile

config = configparser.ConfigParser()
config.read('fanSpeed.ini')
fanSpeed = config['fanSpeed']
fanMinSpeed = fanSpeed.getint('fanMinSpeed', fallback=0)
fanMaxSpeed = fanSpeed.getint('fanMaxSpeed', fallback=50)
offTemp = fanSpeed.getfloat('offTemp', fallback=40.0)
minTemp = fanSpeed.getfloat('minTemp', fallback=55.0)
maxTemp = fanSpeed.getfloat('maxTemp', fallback=75.0)
tempRange = (maxTemp - offTemp)

# I2C channel 1 is connected to the GPIO pins
comms = config['comms']
channel = comms.getint('channel', fallback=1)
# Initialize I2C (SMBus)
bus = SMBus(channel)
#  Arduino addr
address = int(comms.get('address'), 16)
# Protocol values
startMsg = int(comms.get('startMsg', fallback='0x55'), 16)
endMsg = int(comms.get('endMsg', fallback='0xaa'), 16)
noFanSpeed = int(comms.get('noFanSpeed', fallback=0xff), 16)

# Shell command params to get host temp
shellConfig = config['shell']
tempCommand = shellConfig['tempCommand']
tempResultFormat = shellConfig['tempResultFormat']
tempResultParser = compile(tempResultFormat)
userName = shellConfig['userName']

# Read hosts.ini file
config = configparser.ConfigParser()
config.read('hosts.ini')
hosts = config['hosts']
hostList = []
for slot in hosts:
    hostInSlot = hosts[slot]
    if (hostInSlot != 'EMPTY'):
        hostList.append(hostInSlot) #List of hosts to get temp of
fans = config['fans']
fanByHost = dict()  #Key fan number associated with a host
for slot in fans:
    hostInSlot = hosts[slot]
    if (hostInSlot != 'EMPTY'):
        fanByHost[hostInSlot] = int(fans[slot])

lastFanState = [0, 0, 0, 0]

def sendFanSpeed(speeds):
    # Create a message sending the desired fan speed for each fan as a byte
    msg = [startMsg]
    msg.extend(speeds)
    msg.append(endMsg)

    attempts = 0
    sent = False
    while (not sent and attempts < 3):
        try: 
            attempts += 1
            # Write out I2C command: address, offset, msg
            bus.write_i2c_block_data(address, 0, msg)
            sent = True
            print (f"Sent: {msg}")
        except OSError:
            sleep(0.5) # Wait then retry
            print(f"Failed to send message {msg}")


def getHostTempRemote(host):
    temp = 0.0 #No temp measured
    try:
        with Connection(host, user=userName) as conn:
            result = conn.sudo(tempCommand, hide=True)
            tempStr = result.stdout.strip('\n')
            # print(f"Parsing str :{tempStr} with formatter {tempResultFormat}")
            out = tempResultParser.parse(tempStr)
            if (out): temp = out[0]
            # print(f"Temp extracted {temp}")
    except ssh_exception.NoValidConnectionsError:
        pass
    return temp

def getTempByHost(hosts):
    tempByHost = dict()
    groupResult = None
    with ThreadingGroup(*hosts, user=userName) as grp:
        try:
            groupResult = grp.sudo(tempCommand, hide=True)
        except exceptions.GroupException as res:
            groupResult = res.args[0]
            pass
        if (groupResult):
            # successfull = groupResult.succeeded
            for conn in groupResult:
                result = groupResult[conn]
                if isinstance(result, Result): #Only process successful commands
                    tempStr = result.stdout.strip('\n')
                    # print(f"Parsing str :{tempStr} with formatter {tempResultFormat}")
                    out = tempResultParser.parse(tempStr)
                    if (out): tempByHost[conn.host] = out[0]
    return tempByHost

# From the measured CPU temperature determine how fast to spin the fan
# This keeps the fan noise to a minimum based on how hard the CPU is working
# But: it requires a fan with a PWM input.
def calcFanSpeed(fan, temp):
    if (temp == 0):
        speed = noFanSpeed
        lastFanState[fan] = 1
    else:
        if (lastFanState[fan]):
            lowTemp = offTemp # keep fan on until it is below the off temp
        else:
            lowTemp = minTemp #Keep fan off until it is above the min temp
        if (temp <= offTemp):
            lastFanState[fan] = 0
        elif (temp >= minTemp):
            lastFanState[fan] = 1
        if (temp <= lowTemp):
            speed = 0 #Turn Fan off if temp is less than or equal to off temperature
        else:
            speed = fanMinSpeed + int((fanMaxSpeed - fanMinSpeed) * (temp - lowTemp) / tempRange)
        
    return speed

def controlFanSpeed():
    while True:
        tempsByHost = getTempByHost(hostList)
        speeds = [noFanSpeed, noFanSpeed, noFanSpeed, noFanSpeed]
        tempsByFan = [[],[],[],[]]
        for host in hostList:
            # Read each host temperature and calculate fan speed
            temp = tempsByHost.get(host, 0.0)
            fan = fanByHost[host]
            tempsByFan[fan-1].append(temp)
        print (f"Measured Temps: {tempsByFan}")
        for fan in range(0, len(speeds)):
            # Calc fan speed for max temp for hosts cooled by a fan
            speeds[fan] = calcFanSpeed(fan, max(tempsByFan[fan]))
            
        # Set fan speed
        sendFanSpeed(speeds)
        sleep(2.0) # Update every couple of seconds


if __name__ == "__main__":
    controlFanSpeed()    