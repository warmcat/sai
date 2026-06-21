/*
 * Lightweight JS Terminal Emulator Core
 */

class TerminalCore {
    constructor(cols = 80, rows = 24) {
        this.cols = cols;
        this.rows = rows;
        this.buffer = []; // Array of { char: ' ', fg: null, bg: null, bold: false }
        this.scrollback = []; // Array of pushed-out rows
        this.maxScrollback = 10000;
        this.scrollOffset = 0; // Number of lines scrolled back (0 = bottom)
        this.cx = 0;
        this.cy = 0;
        
        this.defaultCell = { char: ' ', fg: null, bg: null, bold: false };
        this.currentStyle = { fg: null, bg: null, bold: false };
        
        this.initBuffer();
    }

    initBuffer() {
        this.wrapNext = false;
        this.buffer = [];
        for (let r = 0; r < this.rows; r++) {
            let row = [];
            for (let c = 0; c < this.cols; c++) {
                row.push({...this.defaultCell});
            }
            this.buffer.push(row);
        }
    }

    resize(cols, rows) {
        this.wrapNext = false;
        let newBuffer = [];
        for (let r = 0; r < rows; r++) {
            let row = [];
            for (let c = 0; c < cols; c++) {
                if (r < this.rows && c < this.cols) {
                    row.push(this.buffer[r][c]);
                } else {
                    row.push({...this.defaultCell});
                }
            }
            newBuffer.push(row);
        }
        this.buffer = newBuffer;
        this.cols = cols;
        this.rows = rows;
        if (this.cx >= cols) this.cx = cols - 1;
        if (this.cy >= rows) this.cy = rows - 1;
    }

    write(data) {
        if (this.scrollOffset > 0 && data.length > 0) {
            this.scrollOffset = 0;
        }
        for (let i = 0; i < data.length; i++) {
            let char = data[i];

            if (char === '\n') {
                this.cy++;
                this.cx = 0;
                this.wrapNext = false;
                if (this.cy >= this.rows) {
                    this.scrollUp();
                    this.cy = this.rows - 1;
                }
            } else if (char === '\r') {
                this.cx = 0;
                this.wrapNext = false;
            } else if (char === '\b') {
                if (this.cx > 0) this.cx--;
                this.wrapNext = false;
            } else if (char === '\t') {
                this.cx = (this.cx + 8) - (this.cx % 8);
                if (this.cx >= this.cols) {
                    this.cx = this.cols - 1;
                    this.wrapNext = true;
                } else {
                    this.wrapNext = false;
                }
            } else if (char === '\x07') {
                // Bell
            } else if (char === '\x1b') { // ESC
                if (i + 1 < data.length && data[i + 1] === '[') {
                    // CSI
                    let j = i + 2;
                    let paramStr = "";
                    while (j < data.length && (data[j] >= '0' && data[j] <= '9' || data[j] === ';' || data[j] === '?')) {
                        paramStr += data[j];
                        j++;
                    }
                    if (j < data.length) {
                        let cmd = data[j];
                        this.handleCSI(cmd, paramStr);
                        i = j;
                    }
                } else if (i + 1 < data.length && data[i+1] === ']') {
                    // OSC
                    let j = i + 2;
                    while (j < data.length && data[j] !== '\x07' && data[j] !== '\x1b') j++;
                    if (j < data.length && data[j] === '\x1b' && j + 1 < data.length && data[j+1] === '\\') j++;
                    i = j;
                } else if (i + 1 < data.length && (data[i+1] === '(' || data[i+1] === ')' || data[i+1] === '*' || data[i+1] === '+')) {
                    // Designate G0-G3 Character Set (3 bytes)
                    i += 2;
                } else {
                    i++; // Skip unknown 2-byte ESC sequence
                }
            } else {
                if (this.wrapNext) {
                    this.cx = 0;
                    this.cy++;
                    this.wrapNext = false;
                    if (this.cy >= this.rows) {
                        this.scrollUp();
                        this.cy = this.rows - 1;
                    }
                }
                
                if (this.cy >= this.rows) this.cy = this.rows - 1;

                this.buffer[this.cy][this.cx] = {
                    char: char,
                    fg: this.currentStyle.fg,
                    bg: this.currentStyle.bg,
                    bold: this.currentStyle.bold
                };
                
                if (this.cx < this.cols - 1) {
                    this.cx++;
                } else {
                    this.wrapNext = true;
                }
            }
        }
    }

    handleCSI(cmd, paramStr) {
        if (paramStr.startsWith('?')) paramStr = paramStr.substring(1);
        let params = paramStr.split(';').map(p => parseInt(p, 10));
        
        if (cmd !== 'm') {
            this.wrapNext = false;
        }

        switch (cmd) {
            case 'A': // Cursor Up
                this.cy = Math.max(0, this.cy - (params[0] || 1)); break;
            case 'B': // Cursor Down
                this.cy = Math.min(this.rows - 1, this.cy + (params[0] || 1)); break;
            case 'C': // Cursor Forward
                this.cx = Math.min(this.cols - 1, this.cx + (params[0] || 1)); break;
            case 'D': // Cursor Back
                this.cx = Math.max(0, this.cx - (params[0] || 1)); break;
            case 'G': // Cursor Horizontal Absolute
            case '`':
                this.cx = Math.min(this.cols - 1, Math.max(0, (params[0] || 1) - 1)); break;
            case 'd': // Vertical Position Absolute
                this.cy = Math.min(this.rows - 1, Math.max(0, (params[0] || 1) - 1)); break;
            case 'H':
            case 'f': // Cursor Position
                this.cy = Math.min(this.rows - 1, Math.max(0, (params[0] || 1) - 1));
                this.cx = Math.min(this.cols - 1, Math.max(0, (params[1] || 1) - 1));
                break;
            case 'J': // Erase in Display
                let modeJ = params[0] || 0;
                if (modeJ === 0) {
                    for (let c = this.cx; c < this.cols; c++) this.buffer[this.cy][c] = {...this.defaultCell};
                    for (let r = this.cy + 1; r < this.rows; r++) {
                        for (let c = 0; c < this.cols; c++) this.buffer[r][c] = {...this.defaultCell};
                    }
                } else if (modeJ === 1) {
                    for (let c = 0; c <= this.cx; c++) this.buffer[this.cy][c] = {...this.defaultCell};
                    for (let r = 0; r < this.cy; r++) {
                        for (let c = 0; c < this.cols; c++) this.buffer[r][c] = {...this.defaultCell};
                    }
                } else if (modeJ === 2) {
                    this.initBuffer();
                }
                break;
            case 'K': // Erase in Line
                let modeK = params[0] || 0;
                if (modeK === 0) {
                    for (let c = this.cx; c < this.cols; c++) this.buffer[this.cy][c] = {...this.defaultCell};
                } else if (modeK === 1) {
                    for (let c = 0; c <= this.cx; c++) this.buffer[this.cy][c] = {...this.defaultCell};
                } else if (modeK === 2) {
                    for (let c = 0; c < this.cols; c++) this.buffer[this.cy][c] = {...this.defaultCell};
                }
                break;
            case '@': // Insert Character
                let nInsert = params[0] || 1;
                for (let c = this.cols - 1; c >= this.cx + nInsert; c--) {
                    this.buffer[this.cy][c] = {...this.buffer[this.cy][c - nInsert]};
                }
                for (let c = this.cx; c < this.cx + nInsert && c < this.cols; c++) {
                    this.buffer[this.cy][c] = {...this.defaultCell};
                }
                break;
            case 'P': // Delete Character
                let nDelete = params[0] || 1;
                if (nDelete > this.cols - this.cx) nDelete = this.cols - this.cx;
                for (let c = this.cx; c < this.cols - nDelete; c++) {
                    this.buffer[this.cy][c] = {...this.buffer[this.cy][c + nDelete]};
                }
                for (let c = Math.max(this.cx, this.cols - nDelete); c < this.cols; c++) {
                    this.buffer[this.cy][c] = {...this.defaultCell};
                }
                break;
            case 'X': // Erase Character
                let nErase = params[0] || 1;
                for (let c = this.cx; c < this.cx + nErase && c < this.cols; c++) {
                    this.buffer[this.cy][c] = {...this.defaultCell};
                }
                break;
            case 'm': // SGR
                for (let i = 0; i < params.length; i++) {
                    let code = params[i] || 0;
                    if (code === 0) this.currentStyle = { fg: null, bg: null, bold: false };
                    else if (code === 1) this.currentStyle.bold = true;
                    else if (code >= 30 && code <= 37) this.currentStyle.fg = code - 30;
                    else if (code >= 40 && code <= 47) this.currentStyle.bg = code - 40;
                }
                break;
        }
    }

    scrollUp() {
        let shiftedRow = this.buffer.shift();
        this.scrollback.push(shiftedRow);
        if (this.scrollback.length > this.maxScrollback) {
            this.scrollback.shift();
        }
        let row = [];
        for (let c = 0; c < this.cols; c++) row.push({...this.defaultCell});
        this.buffer.push(row);
    }

    scroll(amount) {
        this.scrollOffset += amount;
        if (this.scrollOffset < 0) this.scrollOffset = 0;
        if (this.scrollOffset > this.scrollback.length) this.scrollOffset = this.scrollback.length;
    }

    renderHtml() {
        let html = '';
        let lastStyle = null;

        const colorMap = ['black', 'red', 'green', 'yellow', 'blue', 'magenta', 'cyan', 'white'];

        let viewBuffer = [];
        if (this.scrollOffset > 0) {
            let startIdx = this.scrollback.length - this.scrollOffset;
            for (let i = 0; i < this.rows; i++) {
                let idx = startIdx + i;
                if (idx < this.scrollback.length) {
                    viewBuffer.push(this.scrollback[idx]);
                } else {
                    viewBuffer.push(this.buffer[idx - this.scrollback.length]);
                }
            }
        } else {
            viewBuffer = this.buffer;
        }

        for (let r = 0; r < this.rows; r++) {
            for (let c = 0; c < this.cols; c++) {
                let cell = viewBuffer[r][c];
                
                let isCursor = (this.scrollOffset === 0 && r === this.cy && c === this.cx);
                let styleKey = `${cell.fg}-${cell.bg}-${cell.bold}-${isCursor}`;
                
                if (styleKey !== lastStyle) {
                    if (lastStyle !== null) html += '</span>';
                    
                    let classes = [];
                    if (cell.fg !== null) classes.push(`ansi-fg-${colorMap[cell.fg]}`);
                    if (cell.bg !== null) classes.push(`ansi-bg-${colorMap[cell.bg]}`);
                    if (cell.bold) classes.push(`ansi-bold`);
                    if (isCursor) classes.push(`term-cursor`);
                    
                    if (classes.length > 0) {
                        html += `<span class="${classes.join(' ')}">`;
                    } else {
                        html += `<span>`;
                    }
                    lastStyle = styleKey;
                }
                html += (cell.char === ' ' || cell.char === '') ? '&nbsp;' : this.escapeHtml(cell.char);
            }
            if (r < this.rows - 1) html += '<br>';
        }
        if (lastStyle !== null) html += '</span>';
        return html;
    }

    escapeHtml(s) {
        return s.replace(/[&<>"']/g, function(m) {
            return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#039;' }[m];
        });
    }
}
