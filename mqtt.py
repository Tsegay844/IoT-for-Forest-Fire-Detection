import paho.mqtt.client as mqtt
import datetime
import json 
import pymysql

# Database connection 
db = pymysql.connect(host="localhost", user="root", password="root", db="readings")
cursor = db.cursor()

# The callback for when the client receives a CONNACK response from the server.
def on_connect(client, userdata, flags, rc):

    print("Connected with result code " + str(rc))

    # Subscribing in on_connect() means that if we lose the connection and

    # reconnect then subscriptions will be renewed.

    client.subscribe("Data")

# The callback for when a PUBLISH message is received from the server.
def on_message(client, userdata, msg):
    # Skip processing for empty messages
    if not msg.payload:
        print("Received an empty message.")
        return 
        
    timestamp = datetime.datetime.now()
    message_string = msg.payload.decode('utf-8')
 
    try:
        message_data = json.loads(message_string)
        print(f"Message received at {timestamp} || {message_data}")
        sensor_id = message_data.get('sensor_id', 'Unknown Sensor ID')
        readings = message_data.get('readings', [])
        if len(readings) >= 4:
            temperature = readings[0]
            humidity = readings[1]
            CO_level = readings[2]
            light_intensity = readings[3]
            # Insert data into database
            query = """
                INSERT INTO data (sensor_id, temperature, humidity, CO_level, light_intensity, incoming_timestamp)
                VALUES (%s, %s, %s, %s, %s, %s)
            """
            values = (sensor_id, temperature, humidity, CO_level, light_intensity, timestamp)
            cursor.execute(query, values)
            db.commit()
            #print(f"sensor_id: {sensor_id}")
            #print(f"Temperature: {temperature}°C")
            #print(f"Humidity: {humidity}%")
            #print(f"CO Level: {CO_level} ppm")
            print(f"Data inserted into database succefully!")
        else:
            print("Insufficient data in readings.")	
            
    except json.JSONDecodeError as e:
        print("Error decoding JSON:", e)
        print("Faulty payload:", message_string) 

client = mqtt.Client()

client.on_connect = on_connect

client.on_message = on_message

# Connect to the MQTT server
client.connect("127.0.0.1", 1883, 60)

# Loop forever, to maintain continual connection
client.loop_forever()




