var disableSendState = true;

function init() {
    loadSettings();
    loadState();
    setInterval(loadState, 5000);
}

function loadState() {
    $.get( "/api/state", function( data ) {
        disableSendState = true;
        $( "#battery-voltage" ).text(data["batteryVoltage"].toFixed(2));
        $( "#usb-powered" ).text(data["usbPowered"] ? "yes" : "no");
        $( "#wifi-rssi" ).text(data["wifiRssi"]);
        $( "#brightness" ).slider("value", data["brightness"]);
        disableSendState = false;
    });
}

function loadSettings() {
    $.get( "/api/settings", function( data ) {
        $( "#vmix-host" ).val(data["vmixHost"]);
        $( "#vmix-port" ).val(data["vmixPort"]);
        $( "#light-index" ).val(data["lightIndex"]);
    });
}

function save() {
    const data = {
        vmixHost: $("#vmix-host").val(),
        vmixPort: parseInt($("#vmix-port").val()),
        lightIndex: parseInt($("#light-index").val())
    };
    $.ajax({
        type: "POST",
        url: "/api/settings",
        data: JSON.stringify(data),// now data come in this function
        contentType: "application/json; charset=utf-8",
        dataType: "json",
        success: function (data, status, jqXHR) {

            console.log("success");// write success in " "
        },

        error: function (jqXHR, status) {
            // error handler
            console.log('fail' + status.code);
        }
     });
}

function sendState() {
    if (disableSendState) {
        return;
    }
    const data = {
        brightness: parseInt($("#brightness").slider("value"))
    };
    $.ajax({
        type: "POST",
        url: "/api/state",
        data: JSON.stringify(data),// now data come in this function
        contentType: "application/json; charset=utf-8",
        dataType: "json",
        success: function (data, status, jqXHR) {

            console.log("success");// write success in " "
        },

        error: function (jqXHR, status) {
            // error handler
            console.log('fail' + status.code);
        }
     });
}
