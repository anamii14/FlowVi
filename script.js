// ===============================
// ESP32 LIVE DATA
// ===============================

const ESP32_URL = "http://192.168.4.1/data";
const BAG_CAPACITY = 500;


// ===============================
// ALERT SYSTEM
// ===============================

let lastStatus = "";

function addAlert(message, type) {

    let alertList = document.getElementById("alertList");

    let newAlert = document.createElement("div");

    newAlert.className = "alert " + type;

    let currentTime = new Date().toLocaleTimeString();

    newAlert.innerHTML = `
        <span>●</span>
        <div>
            <strong>${message}</strong>
            <p>${currentTime}</p>
        </div>
    `;

    alertList.prepend(newAlert);
}


// ===============================
// STATUS UPDATE
// ===============================

function updateStatus(status) {

    const indicator = document.getElementById("statusIndicator");

    document.getElementById("status").innerText = status;

    if (status === "NORMAL") {

        indicator.innerText = "● Normal";
        indicator.className = "status-indicator normal";

    }

    else if (status === "LOW") {

        indicator.innerText = "● Low Level";
        indicator.className = "status-indicator low";

    }

    else if (status === "CRITICAL") {

        indicator.innerText = "● Critical";
        indicator.className = "status-indicator critical";

    }

    else {

        indicator.innerText = "● Unknown";
        indicator.className = "status-indicator";
    }


    // Add alert only when status changes

    if (status !== lastStatus) {

        if (status === "LOW") {
            addAlert(
                "IV fluid level or flow is low",
                "low-alert"
            );
        }

        else if (status === "CRITICAL") {
            addAlert(
                "Critical IV condition detected",
                "critical-alert"
            );
        }

        else if (status === "NORMAL" && lastStatus !== "") {
            addAlert(
                "IV monitoring returned to normal",
                "normal-alert"
            );
        }

        lastStatus = status;
    }
}


// ===============================
// FLUID LEVEL GRAPH
// ===============================

const ctx = document.getElementById("fluidChart");

const fluidChart = new Chart(ctx, {

    type: "line",

    data: {

        labels: [],

        datasets: [{
            label: "IV Fluid Level (mL)",
            data: [],
            borderWidth: 2,
            tension: 0.3,
            fill: false
        }]

    },

    options: {

        responsive: true,

        scales: {

            y: {
                beginAtZero: true,

                title: {
                    display: true,
                    text: "Fluid Level (mL)"
                }
            },

            x: {

                title: {
                    display: true,
                    text: "Time"
                }
            }
        }
    }
});


// ===============================
// UPDATE GRAPH
// ===============================

function updateChart(level) {

    const currentTime =
        new Date().toLocaleTimeString([], {
            hour: "2-digit",
            minute: "2-digit",
            second: "2-digit"
        });

    fluidChart.data.labels.push(currentTime);

    fluidChart.data.datasets[0].data.push(level);


    // Keep only latest 15 readings

    if (fluidChart.data.labels.length > 15) {

        fluidChart.data.labels.shift();

        fluidChart.data.datasets[0].data.shift();
    }


    fluidChart.update();
}


// ===============================
// UPDATE DASHBOARD
// ===============================

function updateDashboard(data) {

    const ivLevel = Number(data.ivLevel);
    const flowRate = Number(data.flowRate);
    const flowTrend = Number(data.flowTrend);
    const remainingTime = Number(data.remainingTime);
    const status = data.status;


    // IV fluid level

    document.getElementById("ivLevel").innerText =
        ivLevel.toFixed(1) + " mL";


    // Flow rate

    document.getElementById("flowRate").innerText =
        flowRate.toFixed(2) + " mL/min";


    // Flow trend

    document.getElementById("flowTrend").innerText =
        flowTrend.toFixed(3);


    // Remaining time

    document.getElementById("remainingTime").innerText =
        remainingTime.toFixed(1) + " min";


    // Status

    updateStatus(status);


    // Fluid level bar

    let percentage = (ivLevel / BAG_CAPACITY) * 100;
    
    percentage = Math.max(0, Math.min(100, percentage));

    document.getElementById("fluidBar").style.width =
        percentage + "%";


    // Update graph

    updateChart(ivLevel);
}


// ===============================
// FETCH ESP32 DATA
// ===============================

async function fetchESP32Data() {

    try {

        const response = await fetch(ESP32_URL);

        if (!response.ok) {
            throw new Error("ESP32 response error");
        }

        const data = await response.json();

        updateDashboard(data);


        // Connection status

        document.getElementById("connectionStatus").innerHTML =
            "ESP32 Status: <strong>● Connected</strong>";

        document.getElementById("connectionStatus").className =
            "connected";

    }

    catch (error) {

        console.log("ESP32 connection error:", error);

        document.getElementById("connectionStatus").innerHTML =
            "ESP32 Status: <strong>● Disconnected</strong>";

        document.getElementById("connectionStatus").className =
            "disconnected";
    }
}


// ===============================
// START LIVE MONITORING
// ===============================

fetchESP32Data();

setInterval(fetchESP32Data, 2000);