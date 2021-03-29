// this script gets loaded once the page is ready
$(document).ready(function () {

    // some jQuery to handle clicking the menu links
    $('ul.sidenav li').click(function () {
        var index = $(this).attr('data-index');
        $('.content').addClass('hidden');
        $('#' + index).removeClass('hidden');
        $('ul.sidenav li a').removeClass('active').addClass('inactive');
        $('a', this).removeClass('inactive').addClass('active');
    });

    // message submit clicked
    $("#submit").click(function (e) {
        e.preventDefault();
        // send messages to server and update result with server data
        $.get("message", { Name: $('#nameInput')[0].value, Message: $('#messageInput')[0].value }, function (data) {
            $("#result").html(data);
            $("#result").scrollTop(1E10);
        });
    });

    // refresh clicked
    $("#refresh").click(function (e) {
        e.preventDefault();
        $.get("refresh", function (data) {
            $("#result").html(data);
        });
    });

    // login button clicked 
    $("#loginbtn").click(function (e) {
        e.preventDefault();
        // get messages from server
        $.get("login", { Username: $('#usernameInput')[0].value, Password: $('#passwordInput')[0].value}, function (data) {
            $("#private").html(data);
            $("#private").removeClass("hidden");
            $("#refresh").click();
        });
    });

    // users button clicked 
    $("#users").click(function (e) {
        e.preventDefault();
        // get messages from server
        $.get("users", function (data) {
            $("#public").html(data);
            $("#public").removeClass("hidden");
            $("#refresh").click();
        });
    });

    // countries button clicked 
    $("#countries").click(function (e) {
        e.preventDefault();
        // get messages from server
        $.get("countries", function (data) {
            $("#public").html(data);
            $("#public").removeClass("hidden");
            $("#refresh").click();
        });
    });

    // popularity button clicked 
    $("#popularity").click(function (e) {
        e.preventDefault();
        // get messages from server
        $.get("popularity", function (data) {
            $("#public").html(data);
            $("#public").removeClass("hidden");
            $("#refresh").click();
        });
    });

    $("#refresh").click();
});