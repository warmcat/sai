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
        
        let titleLeft = document.createElement('div');
        titleLeft.style.display = 'flex';
        titleLeft.style.alignItems = 'center';

        this.burgerBtn = document.createElement('button');
        this.burgerBtn.className = 'sai-terminal-burger';
        this.burgerBtn.innerHTML = '&#9776;';
        
        this.burgerMenu = document.createElement('div');
        this.burgerMenu.className = 'sai-terminal-menu';
        
        this.menuItemDark = document.createElement('div');
        this.menuItemDark.className = 'sai-terminal-menu-item';
        this.menuItemDark.textContent = 'Theme: Dark';
        this.menuItemDark.onclick = () => { this.setTheme('dark'); this.burgerMenu.classList.remove('show'); };
        
        this.menuItemLight = document.createElement('div');
        this.menuItemLight.className = 'sai-terminal-menu-item';
        this.menuItemLight.textContent = 'Theme: Light';
        this.menuItemLight.onclick = () => { this.setTheme('light'); this.burgerMenu.classList.remove('show'); };
        
        this.burgerMenu.appendChild(this.menuItemDark);
        this.burgerMenu.appendChild(this.menuItemLight);
        
        this.burgerBtn.onclick = (e) => {
            e.stopPropagation();
            this.burgerMenu.classList.toggle('show');
            if (this.updateFocusState) this.updateFocusState();
        };
        
        const closeMenuListener = (e) => {
            if (this.el && !this.burgerBtn.contains(e.target) && !this.burgerMenu.contains(e.target)) {
                if (this.burgerMenu.classList.contains('show')) {
                    this.burgerMenu.classList.remove('show');
                    if (this.updateFocusState) this.updateFocusState();
                }
            }
        };
        document.addEventListener('click', closeMenuListener);
        this._closeMenuListener = closeMenuListener;
        
        titleLeft.appendChild(this.burgerBtn);
        this.titleBar.appendChild(this.burgerMenu);
        
        if (this.options.platform) {
            let plat_parts = this.options.platform.split('/');
            let plat_os = plat_parts[0] || 'generic';
            let plat_arch = plat_parts[1] || 'generic';
            let plat_tc = plat_parts[2] || 'generic';
            
            this.titleIcons = document.createElement('div');
            this.titleIcons.className = 'sai-terminal-icons';
            this.titleIcons.style.display = 'flex';
            this.titleIcons.style.gap = '4px';
            this.titleIcons.style.marginRight = '8px';
            this.titleIcons.innerHTML = 
                `<img class="ip1 zup" src="/sai/${plat_os}.svg">` +
                `<img class="ip1 tread1" src="/sai/arch-${plat_arch}.svg">` +
                `<img class="ip1 tread2" src="/sai/tc-${plat_tc}.svg">`;
            
            const images = this.titleIcons.querySelectorAll('img');
            images.forEach(img => {
                img.onerror = () => {
                    img.src = '/sai/generic.svg';
                    img.onerror = null;
                };
            });
            titleLeft.appendChild(this.titleIcons);
        }
        
        this.titleText = document.createElement('span');
        this.titleText.className = 'sai-terminal-title';
        this.titleText.textContent = this.options.title || "Terminal";
        titleLeft.appendChild(this.titleText);
        
        this.titleBar.appendChild(titleLeft);

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
        
        this.loaderEl = document.createElement('div');
        this.loaderEl.className = 'sai-terminal-loader';
        const spinner = document.createElement('div');
        spinner.className = 'sai-terminal-spinner';
        this.loaderEl.appendChild(spinner);
        this.contentEl.appendChild(this.loaderEl);
        
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
        
        this.resizeObserver = new ResizeObserver(entries => {
            for (let entry of entries) {
                const measureEl = document.createElement('span');
                measureEl.textContent = 'W'.repeat(100);
                measureEl.style.position = 'absolute';
                measureEl.style.visibility = 'hidden';
                measureEl.style.whiteSpace = 'pre';
                this.outputEl.appendChild(measureEl);
                const rect = measureEl.getBoundingClientRect();
                this._charSize = {
                    width: (rect.width / 100) || 8.5,
                    height: Math.max(16, rect.height)
                };
                this.outputEl.removeChild(measureEl);
                
                const cols = Math.max(10, Math.floor(entry.contentRect.width / this._charSize.width));
                const rows = Math.max(5, Math.floor(entry.contentRect.height / this._charSize.height));
                if (cols !== this.core.cols || rows !== this.core.rows) {
                    this.core.resize(cols, rows);
                    this.render();
                    if (this.options.onResize) {
                        this.options.onResize(cols, rows);
                    }
                }
            }
        });
        this.resizeObserver.observe(this.contentEl);

        // Re-evaluate size when custom fonts (like Iosevka) finish loading
        if (document.fonts && document.fonts.ready) {
            document.fonts.ready.then(() => {
                if (this.el.parentNode) {
                    // Force resize observer to re-trigger
                    this.resizeObserver.disconnect();
                    this.resizeObserver.observe(this.contentEl);
                }
            });
        }

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
            if (e.ctrlKey && !e.altKey && !e.metaKey && e.key.length === 1) {
                const charCode = e.key.toLowerCase().charCodeAt(0);
                if (charCode >= 97 && charCode <= 122) {
                    if (charCode === 99 && window.getSelection().toString()) {
                        return; // Let browser natively copy text
                    }
                    if (charCode === 118) { // 'v'
                        return; // Let browser natively paste text
                    }
                    this.options.onData(String.fromCharCode(charCode - 96));
                    e.preventDefault();
                    return;
                }
            }
            if (e.key === 'Enter') {
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
        
        this.inputEl.addEventListener('paste', (e) => {
            let text = (e.clipboardData || window.clipboardData).getData('text');
            if (text) {
                this.options.onData(text);
            }
            e.preventDefault();
        });
        
        this.updateFocusState = () => {
            let menuOpen = this.burgerMenu && this.burgerMenu.classList.contains('show');
            let sel = window.getSelection();
            let hasSelection = sel && !sel.isCollapsed && this.el.contains(sel.anchorNode);
            if (document.activeElement === this.inputEl || isSelecting || menuOpen || hasSelection) {
                this.el.style.opacity = '1';
            } else {
                this.el.style.opacity = '0.7';
            }
        };
        const updateFocusState = this.updateFocusState;

        // click to focus input
        this.el.addEventListener('click', () => {
            let sel = window.getSelection();
            if (!sel || sel.isCollapsed) {
                this.inputEl.focus({ preventScroll: true });
            }
        });
        
        this.inputEl.addEventListener('focus', updateFocusState);
        this.inputEl.addEventListener('blur', updateFocusState);

        // Handle scrolling
        this.contentEl.addEventListener('wheel', (e) => {
            let amount = Math.sign(e.deltaY) * 3;
            if (amount !== 0) {
                this.core.scroll(-amount);
                this.render();
            }
            e.preventDefault();
            e.stopPropagation();
        }, { passive: false });

        let isSelecting = false;
        let selectScrollInterval = null;
        let selectMouseY = 0;

        this.contentEl.addEventListener('mousedown', (e) => {
            isSelecting = true;
            updateFocusState();
        });

        document.addEventListener('mousemove', (e) => {
            if (!isSelecting) return;
            selectMouseY = e.clientY;
            
            const rect = this.contentEl.getBoundingClientRect();
            if (selectMouseY < rect.top) {
                if (!selectScrollInterval) {
                    selectScrollInterval = setInterval(() => {
                        let oldOffset = this.core.scrollOffset;
                        this.core.scroll(1);
                        let diff = this.core.scrollOffset - oldOffset;
                        if (diff !== 0) {
                            let selPos = this.getSelectionPos();
                            this.render();
                            if (selPos) {
                                let shift = diff * (this.core.cols + 1);
                                selPos.anchor += shift;
                                let maxPos = this.core.rows * (this.core.cols + 1);
                                selPos.anchor = Math.max(0, Math.min(maxPos, selPos.anchor));
                                selPos.focus = Math.max(0, Math.min(maxPos, selPos.focus));
                                this.setSelectionPos(selPos);
                            }
                        }
                    }, 50);
                }
            } else if (selectMouseY > rect.bottom) {
                if (!selectScrollInterval) {
                    selectScrollInterval = setInterval(() => {
                        let oldOffset = this.core.scrollOffset;
                        this.core.scroll(-1);
                        let diff = this.core.scrollOffset - oldOffset;
                        if (diff !== 0) {
                            let selPos = this.getSelectionPos();
                            this.render();
                            if (selPos) {
                                let shift = diff * (this.core.cols + 1);
                                selPos.anchor += shift;
                                let maxPos = this.core.rows * (this.core.cols + 1);
                                selPos.anchor = Math.max(0, Math.min(maxPos, selPos.anchor));
                                selPos.focus = Math.max(0, Math.min(maxPos, selPos.focus));
                                this.setSelectionPos(selPos);
                            }
                        }
                    }, 50);
                }
            } else {
                if (selectScrollInterval) {
                    clearInterval(selectScrollInterval);
                    selectScrollInterval = null;
                }
            }
        });

        document.addEventListener('mouseup', () => {
            if (isSelecting) {
                isSelecting = false;
                if (selectScrollInterval) {
                    clearInterval(selectScrollInterval);
                    selectScrollInterval = null;
                }
                updateFocusState();
            }
        });

        // Set initial unfocused opacity
        this.el.style.opacity = '0.7';
        
        // Load theme from localStorage
        this.setTheme(localStorage.getItem('sai-terminal-theme') || 'dark');
        
        // Initial focus
        setTimeout(() => this.inputEl.focus({ preventScroll: true }), 100);
    }

    setTheme(theme) {
        if (theme === 'light') {
            this.el.classList.add('theme-light');
            this.el.classList.remove('theme-dark');
        } else {
            this.el.classList.add('theme-dark');
            this.el.classList.remove('theme-light');
        }
        localStorage.setItem('sai-terminal-theme', theme);
    }

    write(channel, data) {
        if (this.loaderEl) {
            this.loaderEl.remove();
            this.loaderEl = null;
        }
        if (channel === 1 || channel === 2 || channel === 0) {
            this.core.write(data);
            this.render();
        }
    }

    getSelectionPos() {
        const sel = window.getSelection();
        if (!sel.rangeCount || sel.isCollapsed) return null;
        if (!this.outputEl.contains(sel.anchorNode) || !this.outputEl.contains(sel.focusNode)) return null;

        const getPos = (targetNode, targetOffset) => {
            let pos = 0;
            const walk = (node) => {
                if (node === targetNode) {
                    if (node.nodeType === Node.TEXT_NODE) return pos + targetOffset;
                    let childNodes = node.childNodes;
                    for (let i = 0; i < targetOffset && i < childNodes.length; i++) {
                        if (childNodes[i].nodeType === Node.TEXT_NODE) pos += childNodes[i].length;
                        else if (childNodes[i].nodeName === 'BR') pos += 1;
                        else pos += childNodes[i].textContent.length;
                    }
                    return pos;
                }
                if (node.nodeType === Node.TEXT_NODE) {
                    pos += node.length;
                } else if (node.nodeName === 'BR') {
                    pos += 1;
                } else {
                    for (let child of node.childNodes) {
                        let res = walk(child);
                        if (res !== null) return res;
                    }
                }
                return null;
            };
            return walk(this.outputEl);
        };
        
        let a = getPos(sel.anchorNode, sel.anchorOffset);
        let f = getPos(sel.focusNode, sel.focusOffset);
        if (a === null || f === null) return null;
        return { anchor: a, focus: f };
    }

    setSelectionPos(selPos) {
        if (!selPos) return;
        const sel = window.getSelection();
        
        const findNodeAndOffset = (targetPos) => {
            let pos = 0;
            let foundNode = null;
            let foundOffset = 0;
            
            const walk = (node) => {
                if (foundNode) return;
                if (node.nodeType === Node.TEXT_NODE) {
                    if (pos + node.length >= targetPos) {
                        foundNode = node;
                        foundOffset = targetPos - pos;
                        return;
                    }
                    pos += node.length;
                } else if (node.nodeName === 'BR') {
                    if (pos === targetPos) {
                        foundNode = node.parentNode;
                        foundOffset = Array.prototype.indexOf.call(node.parentNode.childNodes, node);
                        return;
                    }
                    pos += 1;
                } else {
                    for (let child of node.childNodes) {
                        walk(child);
                    }
                }
            };
            walk(this.outputEl);
            
            if (!foundNode) {
                let last = this.outputEl.lastChild;
                while (last && last.lastChild) last = last.lastChild;
                if (last && last.nodeType === Node.TEXT_NODE) {
                    foundNode = last;
                    foundOffset = last.length;
                } else {
                    foundNode = this.outputEl;
                    foundOffset = this.outputEl.childNodes.length;
                }
            }
            return { node: foundNode, offset: foundOffset };
        };
        
        let a = findNodeAndOffset(selPos.anchor);
        let f = findNodeAndOffset(selPos.focus);
        
        if (a.node && f.node && sel.setBaseAndExtent) {
            sel.setBaseAndExtent(a.node, a.offset, f.node, f.offset);
        }
    }

    render() {
        this.outputEl.innerHTML = this.core.renderHtml();
    }

    focus() {
        this.inputEl.focus({ preventScroll: true });
    }
    
    destroy() {
        this.resizeObserver.disconnect();
        if (this._closeMenuListener) {
            document.removeEventListener('click', this._closeMenuListener);
        }
        if (this.el.parentNode) {
            this.el.parentNode.removeChild(this.el);
        }
    }
}
