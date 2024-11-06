# SDI-12 Substrate Sensor
Instructions for setting up an SDI12 substrate sensor to work over MQTT to Home Assistant with AUTODISCOVERY!

This works with sensors such as the BGT-SEC(Z2) by [BGT Technology](https://www.alibaba.com/product-detail/China-low-price-CE-IP68-SID12_1600643601689.html) available from Alibaba, or alternatively works with the [Aroya Teros12](https://metergroup.com/products/teros-12/) as well.

Installation is recommended by splicing a Grove cable directly, or use a [3-pole Female 3.5mm -> terminal](https://www.aliexpress.com/item/1005002295771551.html) adapter. The use of [M5Stack VH3.96 - 4Pin Transfer Module Unit](https://shop.m5stack.com/products/3-96-transfer-unit) does not seem to work. For the BGT-SEC(Z2), the unshielded cable is ground, white is power (3.6-16v DC) and the red cable is signal?!?

ESPHome does not directly support SDI-12, and the UART wants TX/RX pins to be uniquely defined, so the easiest solution has been the Arduino IDE and MQTT. Simply install the Mosquitto Broker add-on in Home Assistant, by default this will also enable discovery, and these Sketches have been designed around autodiscovery.

Simply open the sketch, modify the applicable details such as WiFi / HOSTNAME_SUFFIX / mqtt_username / mqtt_password and lastly your mqtt_server IP address. Then, flash, and have a look at the serial console to make sure it was able to reach your MQTT server correctly etc and that autodiscovery worked. For production, set DEBUG to false, and re-flash. If you're using a different ESP32, you can modify SDI12_DATA_PIN to suit your circumstances.

If you run into issues needing the SDI-12 library in your Arduino IDE, you can [download it](https://github.com/HarveyBates/ESP32-SDI12) and copy it to your applicable Arduino libraries directory.

You should then have a device show up automatically in Home Assistant:

![image](https://github.com/user-attachments/assets/0d475142-8f57-445c-b64a-a7e7525c0958)

There is also an additional output in accordance with the BGT-SEC(Z2) manual, though I've not found a use for it:

[Manual for BGT-SEC(Z2) Soil Temp. Humi. EC Sensor (SDI-12 Output, Compatible with Teros 12).pdf](https://github.com/user-attachments/files/17642113/Manual.for.BGT-SEC.Z2.Soil.Temp.Humi.EC.Sensor.SDI-12.Output.Compatible.with.Teros.12.pdf)

This is an initial release and further improvements may come in due course. Pull requests are welcome.
