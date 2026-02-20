let playerSymbol = '';
const socket = new WebSocket("wss://181a-2409-40f4-3113-420a-8a22-10a4-fb88-209a.ngrok-free.app");
const cells = document.querySelectorAll('.cell');

socket.onopen = function() {
    console.log("Connected to server");
};

socket.onmessage = function(event) {
    const data = JSON.parse(event.data);
    if (data.type === "assign") {
        playerSymbol = data.symbol;  
        document.getElementById("player").textContent = "You are player " + playerSymbol +"!";
    }
    if (data.type === "update") {
        updateBoard(data.board);
    }
    if (data.type === "result") {
         document.getElementById("message").textContent = data.message;
    }
    if (data.type === "reset") {
    clearBoard();
    document.getElementById("message").textContent = "";
    }
    if (data.type === "opponent_left") {
    document.getElementById("message").textContent = "Opponent disconnected!";
    clearBoard();
    }
};

cells.forEach(cell => {
    cell.addEventListener('click', function() {
        if (this.textContent.trim() === '') {
            socket.send(JSON.stringify({
                type: "move",
                position: Number(this.id)
            }));
        }
    });
});

document.getElementById("reset").addEventListener("click", function () {
    socket.send(JSON.stringify({
        type: "reset"
    }));
});

function updateBoard(board) {
    cells.forEach((cell, index) => {
        cell.textContent = board[index];
    });
}

function clearBoard() {
    cells.forEach(cell => {
        cell.textContent = '';
    });
}


