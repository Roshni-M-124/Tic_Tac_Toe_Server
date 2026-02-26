let playerSymbol = '';
let clearmessage = false;
const socket = new WebSocket("wss://f527-2409-40f4-3103-f44e-124c-1509-8941-b1b0.ngrok-free.app");
const cells = document.querySelectorAll('.cell');

socket.onopen = function() {
    console.log("Connected to server");
};

socket.onmessage = function(event) {
    const data = JSON.parse(event.data);
    if (data.type === "connected") {
        playerSymbol = data.symbol;  
        document.getElementById("player").textContent = playerSymbol;
    }
    if (data.type === "assign") {
        playerSymbol = data.symbol;  
        document.getElementById("player").textContent = playerSymbol;
        document.getElementById("message").textContent = "Player X should start";
        clearmessage = true;
    }
    if (data.type === "update") {
        updateBoard(data.board);
    }
    if (data.type === "result") {
         document.getElementById("message").textContent = data.message;
    }
    if (data.type === "reset") {
    clearBoard();
    document.getElementById("message").textContent = "Player X should start";
    }
    if (data.type === "opponent_left") {
    document.getElementById("message").textContent = "";
    document.getElementById("player").textContent = "Opponent disconnected!";
    playerSymbol = "";
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
        if(clearmessage){
            document.getElementById("message").textContent = "";
            clearmessage = false;
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
