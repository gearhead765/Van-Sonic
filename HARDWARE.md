# Hardware

## Off the Shelf Hardware
- **Board:** [Seeed Studio XIAO ESP32-S3](https://www.digikey.com/en/products/detail/seeed-technology-co-ltd/102010573/24814471)
- **Rangefinder:**
    - **A02YYUW:** [Waterproof Ultrasonic Sensor DFRobot SEN0311](https://www.digikey.com/en/products/detail/dfrobot/SEN0311/11202577)
- **Miscellaneous Components:**
    - **DC-DC Converter:** [Recom R-78E5.0-0.5](https://www.digikey.com/en/products/detail/recom-power/R-78E5-0-0-5/2834904) 
    - **Phoenix Wire Connector:** [Phoenix Mini Combicon MC 1840366](https://www.digikey.com/en/products/detail/phoenix-contact/1840366/349178) 
    - **Phoenix Board Connector:** [Phoenix Mini Combicon MC 1844210](https://www.digikey.com/en/products/detail/phoenix-contact/1844210/349195)
    - **HY2.0-4P Board Connector** [HY2.0-4P 2mm Right Angle (Grove compatible)](https://www.digikey.com/en/products/detail/seeed-technology-co-ltd/110990037/5482561)    
    - **Polyfuse:** [1/2Amp PTC Resetable Fuse](https://www.digikey.com/en/products/detail/littelfuse-inc/1812L050-60MR/6234813) 
    - **TVS Diode:** [Littelfuse SZ5KASMC36AT3G](https://www.digikey.com/en/products/detail/littelfuse-inc/SZ5KASMC36AT3G/18716674) 
    - **Reverse Polarity Protection:** [Onsemi SS14](https://www.digikey.com/en/products/detail/onsemi/SS14/965474) 
- **Project Box:**  [Hammond 1551KFLBK](https://www.digikey.com/en/products/detail/hammond-manufacturing/1551KFLBK/2094805)
    - **Screws to Secure Board in Enclosure (2 per Project):** [#2 x 1/4" Thread-Forming Screw](https://www.mcmaster.com/products/90385a311/)
    - **3D Printed Washer (2 per Project):** [5/32"[4mm OD] x 5/64"[2mm ID] x 1/16"[1.5mm Thick]](<CAD Model/3D Printed Washer.STL>)

## Modifications to Off the Shelf Hardware
The Hammond enclosure will need a cutout for the Phoenix power connector.  The top will need a tall opening, while the bottom will need the lip removed in the connector area.  Two small washers can be 3D printed to lift the board just a little so the IMU board will clear the lid.
<br>![](Images/cad1.png)![](Images/cad2.png)

[Box cutout dimensions](<CAD Model/box_machining.pdf>)<br>
[Lid cutout dimensions](<CAD Model/lid_machining.pdf>)

## The PCB
[Kicad](https://www.kicad.org/) was used to design the very simple carrier board for the ESP32 development board.  This eliminated the need for point-to-point wiring and made a much more compact package possible.  You can also construct this project on strip board, dead bug style, or how ever you want.

The majority of the components are in the power supply.  Since this was for use in a vehicle, it needed to operate off of 12V.  The sensor is supplied from the 5V output of the DC-DC converter, and the Xiao ESP32 board uses its onboard converter to run off 3.3V.

[![](Images/van_rangefinder_schematic.png)](Notes/van_rangefinder_Schematic.pdf)

The boards were produced by [OSHPark](https://oshpark.com/shared_projects/d4r7hpFr).  You can get three boards for $12 including shipping.  They are produced in the US and usually take a couple of weeks to arrive.  Of course, all of the Kicad source files are included in the repo if you want to send them to your favorite PCB manufacturer.

![](Images/van_level_pcb.png)

Assembly is straight forward.  Solder the lowest components first.