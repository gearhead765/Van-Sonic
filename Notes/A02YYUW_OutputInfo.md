The A02YYUW UART ultrasonic sensor outputs a 4-byte serial data packet over a standard 9600 bps UART interface (TTL level) with a 1 mm resolution for distances ranging from 3 cm to 450 cm. [1] (https://www.dfrobot.com/product-1935.html), [2] (https://makerhardware.net/knowledge-base/projects/interfacing-a02yyuw-1at-ultrasonic-sensor-with-arduino-uno/)

UART Data Packet Format

When reading from the sensor's TX line, each measurement frame consists of 4 bytes:
• Byte 0 (0xFF): Header/Start byte.
• Byte 1 (High Byte): Upper 8 bits of the distance value.
• Byte 2 (Low Byte): Lower 8 bits of the distance value.
• Byte 3 (Sum Check): Checksum calculated as (Byte 0 + Byte 1 + Byte 2) & 0x00FF.

Distance Calculation Formula

To convert the received data into distance in centimeters:
\(\text{Distance\ (cm)}=\frac{(\text{Byte\ }1\times 256)+\text{Byte\ }2}{10}\)

Key Output Specifications

• Communication Protocol: UART (TTL serial, 9600 bps baud rate, 8 data bits, no parity, 1 stop bit).
• Measurement Range: 3 cm to 450 cm (for flat objects).
• Resolution: 1 mm.
• Response Time: ~100 ms.
• Operating Voltage: 3.3V to 5V DC.

[1] (https://wiki.dfrobot.com/sen0311/)<br>
[2] (https://media.digikey.com/pdf/Data%20Sheets/DFRobot%20PDFs/SEN0311_Web.pdf)<br>
[3] (https://www.hestore.hu/prod_getfile.php?id=14191)<br>
[4] (https://www.dfrobot.com/product-1935.html)
<br>
[1] (https://makerhardware.net/knowledge-base/projects/interfacing-a02yyuw-1at-ultrasonic-sensor-with-arduino-uno/)<br>
[2] (https://www.dfrobot.com/product-1935.html)
