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
        $( "#battery-percentage" ).text(data["batteryPercentage"]);
        $( "#usb-powered" ).text(data["usbPowered"] ? "yes" : "no");
        $( "#is-charging" ).text(data["isCharging"] ? "yes" : "no");
        $( "#wifi-rssi" ).text(data["wifiRssi"]);
        $( "#brightness" ).slider("value", data["brightness"]);
        $( "#appVersion" ).text(data["appVersion"]);
        disableSendState = false;
    });
}

function loadSettings() {
    $.get( "/api/settings", function( data ) {
        $( "#name" ).val(data["name"]);
        $( "#vmix-host" ).val(data["vmixHost"]);
        $( "#vmix-port" ).val(data["vmixPort"]);
        $( "#light-index" ).val(data["lightIndex"]);
        $( "#show-preview-on-front" ).prop("checked", data["showPreviewOnFront"]);
        $( "#show-preview-on-back" ).prop("checked", data["showPreviewOnBack"]);
        document.title = 'Tally: ' + $("#name").val();
    });
}

function save() {
    const data = {
        name: $("#name").val(),
        vmixHost: $("#vmix-host").val(),
        vmixPort: parseInt($("#vmix-port").val()),
        lightIndex: parseInt($("#light-index").val()),
        showPreviewOnFront: $("#show-preview-on-front").is(":checked"),
        showPreviewOnBack: $("#show-preview-on-back").is(":checked")
    };
    document.title = 'Tally: ' + $("#name").val();
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
            console.log('fail: ' + status.code);
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
