;;; ghostty-vt.el --- Ghostty VT terminal emulator  -*- lexical-binding: t -*-

;; Copyright (C) 2026 by dickmao
;;
;; Author: dickie smalls <richard@commandlinesystems.com>
;; Version: 0.0.1
;; URL: https://github.com/dickmao/emacs-ghostty.git
;; Package-Requires: ((emacs "29.1"))

;; This file is not part of GNU Emacs.

;; This file is free software; you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation; either version 2, or (at your option)
;; any later version.

;; This file is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.

(require 'ghostty-vt-module)

(defgroup ghostty-vt nil
  "Ghostty VT terminal emulator."
  :group 'terminals)

(defcustom ghostty-vt-shell (or (getenv "SHELL") "/bin/sh")
  "Shell program for ghostty-vt."
  :type 'string
  :group 'ghostty-vt)

(defcustom ghostty-vt-min-window-width 80
  "Minimum window width."
  :type 'integer
  :group 'ghostty-vt)

(defcustom ghostty-vt-max-scrollback 1000
  "Maximum scrollback lines."
  :type 'integer
  :group 'ghostty-vt)

(defvar-local ghostty-vt--term nil)
(defvar-local ghostty-vt--process nil)
(defvar-local ghostty-vt--cursor-overlay nil)
(defvar-local ghostty-vt-copy-mode nil)
(defvar-local ghostty-vt--pending nil)
(defvar-local ghostty-vt--scrollback-end nil)

(defconst ghostty-vt--keys
  '(return tab backtab iso-lefttab backspace escape
    up down left right
    insert delete home end prior next
    f1 f2 f3 f4 f5 f6 f7 f8 f9 f10 f11 f12
    kp-0 kp-1 kp-2 kp-3 kp-4 kp-5 kp-6 kp-7 kp-8 kp-9
    kp-add kp-subtract kp-multiply kp-divide kp-equal
    kp-decimal kp-separator kp-enter))

(defun ghostty-vt--redraw ()
  (let ((inhibit-read-only t))
    (ghostty-vt--render ghostty-vt--term)))

(defun ghostty-vt--filter (proc data)
  (when-let ((buf (process-buffer proc)))
    (with-current-buffer buf
      (if ghostty-vt-copy-mode
          (push data ghostty-vt--pending)
        (ghostty-vt--write ghostty-vt--term data)
        (ghostty-vt--redraw)))))

(defun ghostty-vt--send-event (event)
  (let* ((modifiers (event-modifiers event))
         (shift (memq 'shift modifiers))
         (meta (memq 'meta modifiers))
         (ctrl (memq 'control modifiers))
         (raw-key (event-basic-type event)))
    (when-let ((key (if (characterp raw-key)
                        (string raw-key)
                      (key-description (vector raw-key)))))
      (when (and (characterp raw-key) shift (not meta) (not ctrl))
        (setq key (upcase key)))
      (ghostty-vt-send-key key shift meta ctrl))))

(defun ghostty-vt-send-key (key &optional shift meta ctrl)
  "Send KEY to the terminal with optional modifiers SHIFT, META, CTRL."
  (deactivate-mark)
  (when ghostty-vt--term
    (let ((inhibit-redisplay t)
          (inhibit-read-only t))
      (when-let ((encoded (ghostty-vt--encode-key
			   ghostty-vt--term key
                           (and shift t) (and meta t) (and ctrl t))))
        (when (> (length encoded) 0)
          (process-send-string ghostty-vt--process encoded))))))

(defun ghostty-vt-send (key)
  "Send KEY to the terminal.  KEY can be anything `kbd' understands."
  (mapc #'ghostty-vt--send-event (listify-key-sequence (kbd key))))

(defun ghostty-vt-send-next-key ()
  "Read next input event and send it to the terminal."
  (interactive)
  (ghostty-vt--send-event (read-event)))

(defun ghostty-vt-send-string (string &optional paste-p)
  "Send STRING to the terminal.  If PASTE-P, use bracketed paste."
  (when ghostty-vt--term
    (when paste-p
      (process-send-string ghostty-vt--process "\e[200~"))
    (process-send-string ghostty-vt--process string)
    (when paste-p
      (process-send-string ghostty-vt--process "\e[201~"))))

(defun ghostty-vt--self-insert ()
  (interactive)
  (when ghostty-vt--term
    (ghostty-vt--send-event last-command-event)))

(defun ghostty-vt--alias-set-mark ()
  (interactive)
  (let ((last-command-event (aref (kbd "C-@") 0)))
    (call-interactively #'ghostty-vt--self-insert)))

(defun ghostty-vt--alias-undo ()
  (interactive)
  (let ((last-command-event (aref (kbd "C-_") 0)))
    (call-interactively #'ghostty-vt--self-insert)))


(defun ghostty-vt-next-prompt (n)
  "Move to end of Nth next prompt."
  (interactive "p")
  (unless n (setq n 1))
  (if (< n 0)
      (ghostty-vt-previous-prompt (- n))
    (dotimes (_i n)
      (when-let ((pos (next-single-property-change (point) 'ghostty-vt-prompt)))
        (goto-char pos)))))

(defun ghostty-vt-previous-prompt (n)
  "Move to end of Nth previous prompt."
  (interactive "p")
  (unless n (setq n 1))
  (if (<= n 0)
      (ghostty-vt-next-prompt (- n))
    (dotimes (_i n)
      (when-let ((pos (previous-single-property-change (point) 'ghostty-vt-prompt)))
        (goto-char pos)))))

(defun ghostty-vt-clear ()
  "Clear the terminal screen."
  (interactive)
  (ghostty-vt-send-key "l" nil nil t))

(defun ghostty-vt-yank (&optional _arg)
  "Yank (paste) text into the terminal."
  (interactive "P")
  (deactivate-mark)
  (ghostty-vt-send-string (current-kill 0 t) t))

(defun ghostty-vt-yank-pop (&optional arg)
  "Yank the next entry in the kill ring."
  (interactive "p")
  (ghostty-vt-send-string (current-kill (or arg 1)) t))

(defun ghostty-vt-yank-pop-dwim (&optional arg)
  "Context-aware yank-pop."
  (interactive "p")
  (if (memq last-command '(ghostty-vt-yank ghostty-vt-yank-pop-dwim))
      (ghostty-vt-yank-pop arg)
    (ghostty-vt--self-insert)))

(defun ghostty-vt--prefix-keys ()
  "Return prefix keys that ghostty-vt should not intercept."
  (let (prefix-keys)
    (map-keymap
     (lambda (key binding)
       (when (keymapp binding)
         (let ((key-desc (key-description (vector key))))
           (when (or (not (string-prefix-p "<" key-desc))
                     (not (string-suffix-p ">" key-desc)))
             (push key-desc prefix-keys)))))
     global-map)
    (delete (key-description (vector meta-prefix-char)) prefix-keys)))

(defvar ghostty-vt-mode-map
  (let* ((map (make-keymap))
         (remaps '((scroll-down-command . ghostty-vt--copy-mode-then)
                   (scroll-up-command . ghostty-vt--copy-mode-then)
                   (recenter-top-bottom . ghostty-vt-clear)
                   (beginning-of-buffer . ghostty-vt--copy-mode-then)
                   (end-of-buffer . ghostty-vt--copy-mode-then)
                   (previous-line . ghostty-vt--copy-mode-then)
                   (backward-page . ghostty-vt--copy-mode-then)
                   (forward-page . ghostty-vt--copy-mode-then)
                   (next-line . ghostty-vt--copy-mode-then)))
         (remap-keys
          (mapcar (lambda (pair)
                    (kbd (key-description (where-is-internal
                                           (car pair) nil :first))))
                  remaps)))
    (define-key map [remap self-insert-command] #'ghostty-vt--self-insert)
    (dolist (remap remaps)
      (define-key map (vector 'remap (car remap)) (cdr remap)))
    (dotimes (i 128)
      (let ((key (char-to-string i)))
        (unless (member (kbd (key-description key)) remap-keys)
          (define-key map key 'ghostty-vt--self-insert))))
    (define-key map (kbd "C-SPC") 'ghostty-vt--alias-set-mark)
    (define-key map (kbd "C-/") 'ghostty-vt--alias-undo)
    (define-key map (vector meta-prefix-char) (make-keymap))
    (dotimes (i (1+ (- ?z ?a)))
      (let ((key (vector meta-prefix-char (+ ?a i))))
        (unless (member (kbd (key-description key)) remap-keys)
          (define-key map key 'ghostty-vt--self-insert))))
    (define-key map (kbd "M-y") 'ghostty-vt-yank-pop-dwim)
    (mapc (lambda (key) (define-key map (kbd key) nil)) (ghostty-vt--prefix-keys))
    (define-key map (kbd "C-g") nil)
    (define-key map (where-is-internal #'execute-extended-command nil :first) nil)
    (dolist (key ghostty-vt--keys)
      (define-key map (vector key) #'ghostty-vt--self-insert))
    (dolist (dir '("left" "right" "up" "down"))
      (dolist (mod '("C-" "M-"))
        (define-key map (vector (intern (concat mod dir))) #'ghostty-vt--self-insert)))
    (define-key map [S-prior] #'ghostty-vt--copy-mode-then)
    (define-key map [S-next] #'ghostty-vt--copy-mode-then)
    (dolist (ret '(M-return S-return C-return))
      (define-key map (vector ret) #'ghostty-vt--self-insert))
    (define-key map (kbd "C-c C-c") #'ghostty-vt--self-insert)
    (define-key map (kbd "C-c C-/") #'ghostty-vt--self-insert)
    (define-key map (kbd "C-c C-z") #'ghostty-vt--self-insert)
    (define-key map (kbd "C-c C-d") #'ghostty-vt--self-insert)
    (define-key map (kbd "C-c C-n") #'ghostty-vt-next-prompt)
    (define-key map (kbd "C-c C-p") #'ghostty-vt-previous-prompt)
    (define-key map (kbd "C-c C-t") #'ghostty-vt-copy-mode)
    (define-key map (kbd "C-c C-y") #'ghostty-vt-yank)
    (define-key map (kbd "C-c M-y") #'ghostty-vt-yank-pop)
    map))

(defvar ghostty-vt-copy-mode-map
  (let ((map (make-keymap)))
    (define-key map (kbd "C-c C-t") #'ghostty-vt-copy-mode)
    (define-key map (kbd "C-c C-y") #'ghostty-vt--copy-mode-done-then)
    (define-key map [remap self-insert-command] 'ghostty-vt--copy-mode-done-then)
    (define-key map [return] #'ghostty-vt-copy-mode-done)
    (define-key map (kbd "C-m") #'ghostty-vt-copy-mode-done)
    (define-key map (kbd "C-c C-n") #'ghostty-vt-next-prompt)
    (define-key map (kbd "C-c C-p") #'ghostty-vt-previous-prompt)
    map))

(define-minor-mode ghostty-vt-copy-mode
  "Toggle `ghostty-vt-copy-mode'."
  :group 'ghostty-vt
  :lighter " GhosttyVTCopy"
  :keymap ghostty-vt-copy-mode-map
  (unless (derived-mode-p 'ghostty-vt-mode)
    (user-error "Not a ghostty buffers"))
  (let ((inhibit-read-only t))
    (if ghostty-vt-copy-mode
	(save-excursion
          (setq cursor-type t)
          (use-local-map nil)
          (let ((ws (copy-marker (window-start) t)))
            (setq ghostty-vt--scrollback-end
		  (ghostty-vt--prepend-history ghostty-vt--term))
            (set-window-start nil ws)
	    ;; a gc reclamation thing
            (set-marker ws nil)))
      (delete-region (point-min) ghostty-vt--scrollback-end)
      (setq ghostty-vt--scrollback-end (point-min))
      (use-local-map ghostty-vt-mode-map)
      (setq cursor-type nil)
      (when ghostty-vt--pending
	(mapc (lambda (data) (ghostty-vt--write ghostty-vt--term data))
	      (nreverse ghostty-vt--pending))
	(setq ghostty-vt--pending nil))
      (ghostty-vt--redraw))))

(defun ghostty-vt-copy-mode-done ()
  (interactive)
  (ghostty-vt-copy-mode -1))

(defun ghostty-vt--copy-mode-then ()
  (interactive)
  (let ((keys (key-description (this-command-keys))))
    (call-interactively #'ghostty-vt-copy-mode)
    (when-let ((command (keymap-lookup global-map keys)))
      (call-interactively command))))

(defun ghostty-vt--copy-mode-done-then ()
  (interactive)
  (let ((keys (key-description (this-command-keys))))
    (call-interactively #'ghostty-vt-copy-mode-done)
    (when-let ((command (keymap-lookup ghostty-vt-mode-map keys)))
      (call-interactively command))))

(defun ghostty-vt--adjust-window-size (process windows)
  (let* ((size (funcall window-adjust-process-window-size-function process windows))
         (cols (max (car size) ghostty-vt-min-window-width))
         (rows (cdr size)))
    (when (and (> cols 0) (> rows 0))
      (ghostty-vt--resize ghostty-vt--term rows cols
                          (frame-char-width) (frame-char-height))
      (cons cols rows))))

(define-derived-mode ghostty-vt-mode fundamental-mode "GhosttyVT"
  "Major mode for ghostty-vt."
  (setq-local
   cursor-type nil
   buffer-read-only t
   buffer-undo-list t
   scroll-conservatively 101
   scroll-margin 0
   hscroll-margin 0
   hscroll-step 1
   ghostty-vt--term (ghostty-vt--new
		     (window-body-height)
		     (max (window-max-chars-per-line)
			  ghostty-vt-min-window-width)
		     ghostty-vt-max-scrollback)
   ghostty-vt--process
   (make-process
    :name "ghostty-vt"
    :buffer (current-buffer)
    :command
    `("/bin/sh" "-c"
      ,(format
	"stty -nl sane %s erase ^? rows %d columns %d >/dev/null && TERM=xterm-256color exec %s"
	(if (eq system-type 'berkeley-unix) "" "iutf8")
	(window-body-height)
	(max (window-max-chars-per-line) ghostty-vt-min-window-width)
	ghostty-vt-shell))
    :coding 'no-conversion
    :connection-type 'pty
    :file-handler t
    :filter #'ghostty-vt--filter
    :sentinel (lambda (proc _msg)
                (when (buffer-live-p (process-buffer proc))
                  (kill-buffer (process-buffer proc))))))
  (require 'hl-line)
  (require 'display-line-numbers)
  (dolist (mode '(display-line-numbers-mode hl-line-mode font-lock-mode))
    (let* ((mode-hook (intern-soft (concat (symbol-name mode) "-hook")))
	   (mode-hook-value (ignore-errors (symbol-value mode-hook)))
	   (negatory (lambda ()
		       (cl-letf (((symbol-value mode-hook) mode-hook-value))
			 (funcall mode -1)
			 (user-error "No dude")))))
      (funcall mode -1)
      (add-hook mode-hook negatory nil :local)))
  (add-hook 'change-major-mode-hook
	    (lambda () (user-error "No dude"))
	    nil :local)
  (process-put ghostty-vt--process 'adjust-window-size-function
	       #'ghostty-vt--adjust-window-size)
  (let ((display-table (or buffer-display-table (make-display-table))))
    (set-display-table-slot display-table 'truncation 32)
    (setq buffer-display-table display-table)))

;;;###autoload
(defun ghostty-vt (&optional arg)
  "Open a ghostty-vt terminal buffer."
  (interactive "P")
  (pop-to-buffer-same-window
   (with-current-buffer (if arg (generate-new-buffer "*ghostty-vt*")
			  (get-buffer-create "*ghostty-vt*"))
     (unless (derived-mode-p 'ghostty-vt-mode)
       (ghostty-vt-mode))
     (current-buffer))))

(provide 'ghostty-vt)
;;; ghostty-vt.el ends here
