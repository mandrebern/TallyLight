function init() {
    loadSettings();
    loadState();
}

function loadState() {
    $.get( "/api/state", function( data ) {
        $( "#battery-voltage" ).text(data["batteryVoltage"].toFixed(2));
        $( "#usb-powered" ).text(data["usbPowered"] ? "yes" : "no");
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