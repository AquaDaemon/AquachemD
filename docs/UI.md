<p align="center">
  <img src="../web/aquachemd.png" width="120" alt="AquachemD logo">
</p>

<h1 align="center">AquachemD</h1>
<p align="center"><b>Open-source, automated pool water chemistry — pH, ORP, and dosing, done right.</b></p>

---

[Web / App UI examples](#WebUI)

[Homekit examples](#HomeKit)

[HomeAssistant examples](#HomeAssistant)

[How to customize web config](UI%20Config.md)

---


# Quick Web UI overview
<a id="WebUI"></a>

Full details on all UI options coming soon, below are a few examples. Just about everything is fully customizable.

<img src="images/AquachemD.png" alt="Alt Text">
<br><br>
<hr>

- Each tile can have it's own limits for normal/high/out or range, and also custom text & colors for each, and will show timers / dose time / status etc

<table align="center" border="0" cellpadding="0" cellspacing="0">
  <tr>
    <td><img src="images/sensor.png" alt="Sensor" width="100"></td>
    <td><img src="images/sensor-high.png" alt="High" width="100"></td>
    <td><img src="images/sensor-outrange.png" alt="Out Range" width="100"></td>
    <td><img src="images/sensor-custom.png" alt="Custom" width="100"></td>
    <td><img src="images/dose-time.png" alt="Dose Time" width="100"></td>
    <td><img src="images/disabled.png" alt="Disabled" width="100"></td>
  </tr>
</table>
<br><br>
<hr>

- iPhone app is same interface
<table align="center" border="0" cellpadding="0" cellspacing="0">
  <tr>
    <td><img src="images/AquachemD-iphone.png" alt="Alt Text"  width="300"></td>
  </tr>
</table>
<br><br>
<hr>

- Dosers will show multiple options along with recent history
<table align="center" border="0" cellpadding="0" cellspacing="0">
  <tr>
    <td><img src="images/doser.png" alt="Alt Text"  ></td>
  </tr>
</table>
<br><br>
<hr>

- AquachemD Managment Console. (click the burger icon top left of screen)

<img src="images/Aqmanager.png" alt="Alt Text">
<br><br>
<hr>

- AquachemD Scheduler.  Interface for cron. (click the date in top right of screen)

<img src="images/AquachemD-scheduler.png" alt="Alt Text">
<br><br>
<hr>

- Few examples, showing different limits.

<img src="images/AquachemD-sensor-limits.png" alt="Alt Text">
<br><br>
<img src="images/AquachemD-sensor-limits2.png" alt="Alt Text">

----

# Quick HomeKit overview
<a id="HomeKit"></a>

<img src="images/HK-Main.jpg" alt="Alt Text" width="600">
<br><br>
<img src="images/HK-Doser running.jpg" alt="Alt Text" width="250">
<img src="images/HK-Doser time.jpg" alt="Alt Text" width="350">
<br><br>
<img src="images/HK-Lux.jpg" alt="Alt Text" width="350">
<img src="images/HK-Contact.jpg" alt="Alt Text" width="350">
<img src="images/HK-Temp.jpg" alt="Alt Text" width="350">

----

# Quick HomeAssistant examples
<a id="HomeAssistant"></a>

These are simply the default templates, obviously you can fully customize to how you prefer.

<img src="images/HomeAssistant-Controls.png" alt="Alt Text" width="400">
<img src="images/HomeAssistant-Sensors.png" alt="Alt Text" width="400">

----

## Below is an example using Sensor Readings / Manual readings & Pool Maths (calculations)
<img src="images/HomeAssistant-Chemical.png" alt="Alt Text" width="600">