from smbus2 import SMBus
from time import sleep
import configparser
from fabric2 import Connection, ThreadingGroup, Result, exceptions
from paramiko import ssh_exception
from parse import parse, compile
from datetime import datetime, timedelta
import os

CRC7_POLY = 0x91

config = configparser.ConfigParser()
config.read('fanSpeed.ini')
fanSpeed = config['fanSpeed']
fanSpeedRange = [0, 0, 0, 0]
for f in range(1,5):
    minS = fanSpeed.getint(f'fan{f}MinSpeed', fallback=30)
    maxS = fanSpeed.getint(f'fan{f}MaxSpeed', fallback=50)
    fanSpeedRange[f-1] = (minS, maxS)
fanSupplyVoltage = fanSpeed.getint('fanSupplyVoltage', fallback=120)
pwmOutput = fanSpeed.getint('pwmOutput', fallback=0)
offTemp = fanSpeed.getfloat('offTemp', fallback=40.0)
minTemp = fanSpeed.getfloat('minTemp', fallback=55.0)
maxTemp = fanSpeed.getfloat('maxTemp', fallback=75.0)
tempRange = (maxTemp - offTemp)
timeStr = ""

# I2C channel 1 is connected to the GPIO pins
comms = config['comms']
channel = comms.getint('channel', fallback=1)
# Initialize I2C (SMBus)
bus = SMBus(channel)
#  Arduino addr
address = int(comms.get('address'), 16)
# Protocol values
startMsg = int(comms.get('startMsg', fallback=240))
endMsg = int(comms.get('endMsg', fallback=250))
noFanSpeed = int(comms.get('noFanSpeed', fallback=255))

# Shell command params to get host temp
shellConfig = config['shell']
tempCommand = shellConfig['tempCommand']
tempResultFormat = shellConfig['tempResultFormat']
tempResultParser = compile(tempResultFormat)
userName = shellConfig['userName']

# Read hosts.ini file
# configHosts = configparser.ConfigParser()
# configHosts.read('hosts.ini')
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

def getHostTempRemote(host):
    temp = 0.0 #No temp measured
    try:
        with Connection(host, user=userName) as conn:
            result = conn.run(tempCommand, hide=True, timeout=5)
            tempStr = result.stdout.strip('\n')
            # print(f"Parsing str :{tempStr} with formatter {tempResultFormat}")
            out = tempResultParser.parse(tempStr)
            if (out): temp = out[0]
            # print(f"Temp extracted {temp}")
    except ssh_exception.NoValidConnectionsError:
        print(f"Got NoValidConnectionsError running command {tempCommand} on host {host}")
        pass
    except ssh_exception.SSHException:
        print(f"Got SSH exception running command {tempCommand} on host {host}")
    return temp

def getTempByHostThreaded(hosts, tempByHost):
    if len(hosts) > 0:
        groupResult = None
        with ThreadingGroup(*hosts, user=userName, connect_timeout=10) as grp:
            try:
                groupResult = grp.run(tempCommand, hide=True, timeout=5)
            except exceptions.GroupException as res:
                groupResult = res.args[0]
                print(f"Got GroupException running command: {res}")
            except ssh_exception.NoValidConnectionsError as res:
                groupResult = res.args[0]
                print(f"Got NoValidConnectionsError running command: {res}")
            except ssh_exception.SSHException as res:
                groupResult = res.args[0]
                print(f"Got SSH exception running command: {res}")
            if (groupResult):
                # successfull = groupResult.succeeded
                for conn in groupResult:
                    result = groupResult[conn]
                    if isinstance(result, Result): #Only process successful commands
                        tempStr = result.stdout.strip('\n')
                        # print(f"Remote result: Parsing str :{tempStr} with formatter {tempResultFormat}")
                        out = tempResultParser.parse(tempStr)
                        if (out): tempByHost[conn.host] = out[0]
            else:
                print(f"Failed to run command: {groupResult}")
    return tempByHost

def getTempByHostLocal(hosts: list[str]):
    tempByHost = dict()
    tempDir = config['localResultFiles']['resultlocation']
    nowTime = datetime.now().timestamp()
    for host in hosts:
        resultFile = f"{tempDir}/{host}.txt"
        try:
            with open(resultFile, 'r') as tempFile:
                #Check timestamp, if greater than 2 mins ago, ignore
                if (nowTime - os.stat(resultFile).st_mtime <= 120):
                    tempStr = tempFile.read().strip('\n')
                    # print(f"Local result: Parsing str :{tempStr} with formatter {tempResultFormat}")
                    out = tempResultParser.parse(tempStr)
                    if (out): 
                        tempByHost[host] = out[0]
        except FileNotFoundError:
            #May happen
            pass 
    return tempByHost

# From the measured CPU temperature determine how fast to spin the fan
# This keeps the fan noise to a minimum based on how hard the CPU is working
def calcFanSpeed(fan, temp):
    if (temp == 0):
        speed = noFanSpeed
        lastFanState[fan] = 1
    else:
        (fanMinSpeed, fanMaxSpeed) = fanSpeedRange[fan]
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
        elif (temp >= maxTemp):
            speed = fanMaxSpeed
        else:
            speed = fanMinSpeed + int((fanMaxSpeed - fanMinSpeed) * (temp - offTemp) / tempRange)
        
    return speed

def getRequiredFanSpeeds():
    tempsByHost = getTempByHostLocal(hostList)
    # hosts = [ host for host in hostList if host not in tempsByHost ]
    # tempsByHost = getTempByHostThreaded(hosts, tempsByHost)
    speeds = [noFanSpeed, noFanSpeed, noFanSpeed, noFanSpeed]
    tempsByFan = [[],[],[],[]]
    for host in hostList:
        # Read each host temperature and calculate fan speed
        temp = tempsByHost.get(host, 0.0)
        fan = fanByHost[host]
        tempsByFan[fan-1].append(temp)
    print (f"{timeStr}: Measured Temps: {tempsByFan}")
    for fan in range(0, len(speeds)):
        # Calc fan speed for max temp for hosts cooled by a fan
        speeds[fan] = calcFanSpeed(fan, max(tempsByFan[fan]))
    return speeds

def createPayload(speeds):
    #Create payload sending the desired fan speed for each fan as a byte
    data = []
    for fan in range(0,4):
        (minS, maxS) = fanSpeedRange[fan]
        data.append(minS) # Send speed range so indicator LED can be driven correctly
        data.append(maxS)
        data.append(speeds[fan])
    data.append(pwmOutput)
    data.append(fanSupplyVoltage)
    return data

def getCRC(data):
  crc = 0
  for b in data:
    crc ^= b
    for _ in range(0,8):
        if (crc & 1):
            crc ^= CRC7_POLY
        crc >>= 1
  return crc

def sendMessage(payload):
    # Create a message 
    msg = [startMsg]
    msg.extend(payload)
    msg.append(getCRC(payload))
    msg.append(endMsg)

    attempts = 0
    sent = False
    while (not sent and attempts < 3):
        try: 
            # Write out I2C command: address, offset, msg
            bus.write_i2c_block_data(address, 0, msg)
            sent = True
            #print (f"{timeStr}: Sent: {msg}")
        except OSError:
            attempts += 1
            sleep(1.0) # Wait then retry
            print(f"{timeStr}: Failed to send message {msg}")
    return attempts

if __name__ == "__main__":
    while True:
        n = datetime.now()
        timeStr = n.strftime("%Y/%m/%d %H:%M:%S")
        #Get the speeds to set per fan based on CPU temps
        speeds = getRequiredFanSpeeds()
        #Create message payload
        payload = createPayload(speeds)
        # Send payload to controller
        noOfTries = sendMessage(payload)
        sleep(15.0 - noOfTries) # Update every couple of seconds
