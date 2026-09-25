# FlowVi

**Smart IV Drip Monitoring System** — Real-Time IV Fluid & Flow Monitoring System using ESP32, HX711 Load Cell, and Random Forest Machine Learning.

## Features
- **Real-Time Monitoring**: Tracks IV fluid level, flow rate, and flow trends.
- **Alert System**: Visual and audio alerts (LEDs, Buzzer, I2C LCD, and Web Dashboard).
- **Predictive Intelligence**: Embedded Random Forest model for status classification and remaining time prediction.
- **Web Dashboard**: Responsive web interface with real-time graphs and status tracking.

## Project Structure
- `loadcell_alertsystem.ino` - Arduino/ESP32 firmware for sensors and alert system.
- `random_forest_model_esp32.h` - Trained Random Forest model header for ESP32.
- `index.html` - Monitoring dashboard interface.
- `style.css` - Dashboard styling.
- `script.js` - Dashboard logic and live chart visualization.