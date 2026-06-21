/*
 * Custom Terminal Emulator for Sai
 * 
 * Provides an abstract interface for passing buffers of data in and out,
 * marked with stdxxx.
 */

class SaiTerminal {
    constructor(container, options = {}) {
        this.container = container;
        this.options = Object.assign({
            onData: (data) => {},
            onClose: () => {}
        }, options);

        this.el = document.createElement('div');
        this.el.className = 'sai-terminal';
        
        // Window furniture
        this.titleBar = document.createElement('div');
        this.titleBar.className = 'sai-terminal-titlebar';
        
        this.titleText = document.createElement('span');
        this.titleText.className = 'sai-terminal-title';
        this.titleText.textContent = this.options.title || "Terminal";
        this.titleBar.appendChild(this.titleText);

        this.closeBtn = document.createElement('button');
        this.closeBtn.className = 'sai-terminal-close';
        this.closeBtn.textContent = 'X';
        this.closeBtn.onclick = () => {
            this.options.onClose();
            this.destroy();
        };
        this.titleBar.appendChild(this.closeBtn);
        this.el.appendChild(this.titleBar);

        this.contentEl = document.createElement('div');
        this.contentEl.className = 'sai-terminal-content';
        
        this.outputEl = document.createElement('div');
        this.outputEl.className = 'sai-terminal-output';
        this.contentEl.appendChild(this.outputEl);
        
        this.inputEl = document.createElement('input');
        this.inputEl.className = 'sai-terminal-input';
        this.inputEl.type = 'text';
        // Hide the input, we use it just for focus/typing
        this.inputEl.style.opacity = '0';
        this.inputEl.style.position = 'absolute';
        this.inputEl.style.left = '-9999px';
        this.contentEl.appendChild(this.inputEl);

        this.el.appendChild(this.contentEl);

        this.container.appendChild(this.el);

        this.core = new TerminalCore(80, 24);
        
        // Setup ResizeObserver to dynamically resize
        this.resizeObserver = new ResizeObserver(entries => {
            for (let entry of entries) {
                // Approximate character size (e.g., 8x16)
                // This would be better if dynamically measured
                const cols = Math.max(10, Math.floor(entry.contentRect.width / 8.5));
                const rows = Math.max(5, Math.floor(entry.contentRect.height / 16));
                if (cols !== this.core.cols || rows !== this.core.rows) {
                    this.core.resize(cols, rows);
                    this.render();
                }
            }
        });
        this.resizeObserver.observe(this.contentEl);

        // Dragging logic
        let isDragging = false;
        let dragStartX, dragStartY;
        let startLeft, startTop;

        this.titleBar.addEventListener('mousedown', (e) => {
            if (e.target === this.closeBtn) return;
            isDragging = true;
            dragStartX = e.clientX;
            dragStartY = e.clientY;
            
            const rect = this.el.getBoundingClientRect();
            startLeft = rect.left;
            startTop = rect.top;
            
            this.el.style.left = startLeft + 'px';
            this.el.style.top = startTop + 'px';
            this.el.style.transform = 'none';
        });

        document.addEventListener('mousemove', (e) => {
            if (!isDragging) return;
            const dx = e.clientX - dragStartX;
            const dy = e.clientY - dragStartY;
            this.el.style.left = (startLeft + dx) + 'px';
            this.el.style.top = (startTop + dy) + 'px';
        });

        document.addEventListener('mouseup', () => {
            isDragging = false;
        });

        this.inputEl.addEventListener('keydown', (e) => {
            if (e.ctrlKey) {
                if (e.key === 'c') {
                    this.options.onData('\x03');
                    e.preventDefault();
                } else if (e.key === 'd') {
                    this.options.onData('\x04');
                    e.preventDefault();
                } else if (e.key === 'z') {
                    this.options.onData('\x1a');
                    e.preventDefault();
                } else if (e.key === 'l') {
                    this.options.onData('\x0c');
                    e.preventDefault();
                }
            } else if (e.key === 'Enter') {
                this.options.onData('\r');
                e.preventDefault();
            } else if (e.key === 'Backspace') {
                this.options.onData('\x7f');
                e.preventDefault();
            } else if (e.key === 'Tab') {
                this.options.onData('\t');
                e.preventDefault();
            } else if (e.key === 'Escape') {
                this.options.onData('\x1b');
                e.preventDefault();
            } else if (e.key.startsWith('Arrow')) {
                if (e.key === 'ArrowUp') this.options.onData('\x1b[A');
                else if (e.key === 'ArrowDown') this.options.onData('\x1b[B');
                else if (e.key === 'ArrowRight') this.options.onData('\x1b[C');
                else if (e.key === 'ArrowLeft') this.options.onData('\x1b[D');
                e.preventDefault();
            } else if (e.key.length === 1 && !e.metaKey && !e.altKey) {
                // Send regular characters immediately
                this.options.onData(e.key);
                e.preventDefault();
            }
        });
        
        // click to focus input
        this.el.addEventListener('click', () => {
            const selection = window.getSelection();
            if (!selection.toString()) {
                this.inputEl.focus();
            }
        });
    }

    write(channel, data) {
        if (channel === 1 || channel === 2 || channel === 0) {
            this.core.write(data);
            this.render();
        }
    }

    render() {
        this.outputEl.innerHTML = this.core.renderHtml();
    }

    focus() {
        this.inputEl.focus();
    }
    
    destroy() {
        this.resizeObserver.disconnect();
        if (this.el.parentNode) {
            this.el.parentNode.removeChild(this.el);
        }
    }
}
