function ledStatus (arg) {
    function responseHandler(data, textStatus, jqXHR){
      if(data.status == "ok"){
        $("#response").text("Light is: " + data.led);
      }
      else{
        $("#response").text("Error: " + data.path);
      }
    }
    //all the stuff about getting the status back goes here.
    switch(arg){
      case "on":
        $.post("/led/on", responseHandler);
        break;
      case "off":
        $.post("/led/off", responseHandler);
        break;
      case "toggle":
        $.post("/led/toggle", responseHandler);
        break;
      default:
    }
}

$(document).ready(function(){
    //This handles all buttons. data-func value MUST be the same as function name
    // but for that simple trade off, you get a simple handler.
    $(".actionbutton").click(function(){
        var func = $(this).data("func");
        var args = $(this).data("args");
        window[func](args);
    });
});
