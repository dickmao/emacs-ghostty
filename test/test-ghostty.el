;;; test-ghostty.el --- Tests for ghostty -*- lexical-binding: t; -*-

;; Copyright (C) 2026-3000 dickie smalls

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

;;; Commentary:
;;
;; Tests for ghostty.

;;; Code:

(require 'ert)
(require 'ghostty-vt)

(defvar-local test-ghostty/prompt nil)

(defmacro test-ghostty/with-session (&rest body)
  (declare (indent defun))
  `(let ((b (ghostty-vt :fresh))
	 (wait-refresh (lambda (&rest _args)
			 (accept-process-output ghostty-vt--process 0.05 nil t))))
     (unwind-protect
	 (progn
	   (switch-to-buffer b)
	   (dolist (f '(ghostty-vt-send ghostty-vt-send-key ghostty-vt-send-string))
	     (add-function :after (symbol-function f) wait-refresh))
	   (should (eq ghostty-vt--process (get-buffer-process (current-buffer))))
	   (cl-loop repeat 100
		    until (not (zerop (current-column)))
		    do (sleep-for 0.2)
		    finally
		    (progn (setq test-ghostty/prompt
				 (buffer-substring-no-properties
				  (line-beginning-position) (point)))
			   (should-not (zerop (length test-ghostty/prompt)))))
	   ,@body)
       (dolist (f '(ghostty-vt-send ghostty-vt-send-key ghostty-vt-send-string))
	 (remove-function (symbol-function f) wait-refresh))
       (kill-process ghostty-vt--process)
       (cl-loop repeat 100
		until (not (get-buffer-process b))
		do (sleep-for 0.2)
		finally (should-not (get-buffer-process b)))
       (let (kill-buffer-query-functions)
	 (kill-buffer b)))))

(defsubst test-ghostty/at-prompt ()
  (equal (buffer-substring-no-properties
	  (line-beginning-position) (point))
	 test-ghostty/prompt))

(defsubst test-ghostty/run (command)
  (should (test-ghostty/at-prompt))
  (ghostty-vt-send-string command)
  (ghostty-vt-send-key "<return>")
  (cl-loop repeat 100
	   until (test-ghostty/at-prompt)
	   do (sleep-for 0.2)
	   finally (should (test-ghostty/at-prompt))))

(ert-deftest basic ()
  (test-ghostty/with-session
    (should (eq major-mode 'ghostty-vt-mode))))

(ert-deftest wrap ()
  "noninteractive will always elide every 81st character in continued line,
so x3 cannot be a multiple of window-width, else it'll elide the final newline."
  (test-ghostty/with-session
    (should (= (window-width) ghostty-vt-min-window-width))
    (let ((x3 (make-string (1+ (* 3 (window-width))) ?x)))
      (test-ghostty/run (format "echo %s" x3))
      (save-excursion
	(forward-line -1)
	(let ((pure-text (buffer-substring-no-properties
			  (line-beginning-position) (line-end-position))))
	  (should (equal pure-text x3))))
      (call-interactively #'ghostty-vt-copy-mode)
      (save-excursion
	(forward-line -1)
	(let ((pure-text (buffer-substring-no-properties
			  (line-beginning-position) (line-end-position))))
	  (should (equal pure-text x3)))))))

(ert-deftest contextual-yank-pop ()
  "M-y does different things depending on last-command."
  (test-ghostty/with-session
    (should (zerop (length kill-ring)))
    (should (test-ghostty/at-prompt))
    (test-ghostty/run (format "echo the quick brown fox"))
    (call-interactively #'ghostty-vt-copy-mode)
    (re-search-backward (regexp-quote "brown"))
    (copy-region-as-kill (point) (+ (point) (length "brown")))
    (re-search-backward (regexp-quote "quick"))
    (copy-region-as-kill (point) (+ (point) (length "quick")))
    (call-interactively #'ghostty-vt-copy-mode)
    (should (test-ghostty/at-prompt))
    ;; redraw locks point to cursor but save-excursion should still
    ;; work, right?  Dunno why it doesn't.
    (when-let* ((start (point))
	       (bash-works-p (eq system-type 'gnu/linux)))
      (call-interactively #'ghostty-vt-yank)
      (goto-char start)
      (should (looking-at-p "quick"))
      (setq last-command 'ghostty-vt-yank)
      (call-interactively #'ghostty-vt-yank-pop-dwim)
      (goto-char start)
      (should (looking-at-p "brown"))
      (ghostty-vt-send-key "\C-a")
      (ghostty-vt-send-string "quick ")
      (should (looking-at-p "brown"))
      (ghostty-vt-send-key "\C-a")
      (should (looking-at-p "quick brown"))
      (ghostty-vt-send-key "f" nil t nil) ;M-f
      (should (looking-at-p " brown"))
      (ghostty-vt-send-key "d" nil t nil) ;M-d
      (ghostty-vt-send-key "\C-a")
      (ghostty-vt-send-key "\C-k")
      (should-not (thing-at-point 'word))
      (ghostty-vt-send-key "\C-y")
      (save-excursion
	(goto-char start)
	(should (looking-at-p "quick")))
      (ghostty-vt-send-key "y" nil t nil) ;M-y
      (save-excursion
	(goto-char start)
	(should (looking-at-p " brown"))))))

(ert-deftest trailing-whitespace ()
  "Lines rendered from ghostty have no trailing spaces or tabs."
  (test-ghostty/with-session
    (test-ghostty/run "printf 'foo   \\nbar\\t\\t\\n'")
    (save-excursion
      (re-search-backward "^foo")
      (should (equal (buffer-substring-no-properties
                      (line-beginning-position) (line-end-position))
                     "foo"))
      (forward-line 1)
      (should (equal (buffer-substring-no-properties
                      (line-beginning-position) (line-end-position))
                     "bar")))))

(ert-deftest scrollback ()
  "Scrollback with wrapped lines: viewport is unchanged after copy-mode round-trip."
  (test-ghostty/with-session
    (let* ((wide (make-string (+ 2 (window-width)) ?w))
           (cmd (format "echo %s; for i in $(seq 1 80); do echo line-$i; done" wide)))
      (test-ghostty/run cmd)
      (let ((snapshot (buffer-string)))
        (call-interactively #'ghostty-vt-copy-mode)
        (should (> ghostty-vt--scrollback-end (point-min)))
        (goto-char (point-min))
        (should (search-forward (substring wide 0 20) ghostty-vt--scrollback-end t))
        (call-interactively #'ghostty-vt-copy-mode)
        (should (equal (buffer-string) snapshot))))))

(ert-deftest scrollback-order ()
  "Scrollback pages arrive complete, in order, and styled."
  (test-ghostty/with-session
    (test-ghostty/run "for i in $(seq 1 300); do printf '\\e[31mred-%d\\e[0m plain-%d\\n' $i $i; done")
    (let ((snapshot (buffer-string)))
      (call-interactively #'ghostty-vt-copy-mode)
      (let ((all (buffer-string)) nums (s 0))
        (while (string-match "red-\\([0-9]+\\) plain-\\([0-9]+\\)" all s)
          (should (equal (match-string 1 all) (match-string 2 all)))
          (push (string-to-number (match-string 1 all)) nums)
          (setq s (match-end 0)))
        (setq nums (nreverse nums))
        (should (equal nums (number-sequence 1 300)))
        (goto-char (point-min))
        (re-search-forward "red-[0-9]")
        (should (get-text-property (1- (point)) 'face)))
      (call-interactively #'ghostty-vt-copy-mode)
      (should (equal (buffer-string) snapshot)))))

(ert-deftest isearch-history ()
  "Isearch in copy mode reaches scrollback."
  (test-ghostty/with-session
    (test-ghostty/run "for i in $(seq 1 300); do echo needle-$i; done")
    (call-interactively #'ghostty-vt-copy-mode)
    (goto-char (point-max))
    (should (eq (key-binding (kbd "C-M-r")) 'isearch-backward-regexp))
    (execute-kbd-macro (kbd "C-M-r ^ n e e d l e - 1 2 $ RET"))
    (should (looking-at "needle-12$"))
    (should (< (point) ghostty-vt--scrollback-end))
    (call-interactively #'ghostty-vt-copy-mode)))

(provide 'test-ghostty)
;;; test-ghostty.el ends here

(ert-deftest tty-control-aliases ()
  (let ((term (ghostty-vt--new 24 80 100)))
    (should (equal "\r" (ghostty-vt--encode-key term "m" nil nil t)))
    (should (equal "\t" (ghostty-vt--encode-key term "i" nil nil t)))
    (should (equal "m" (ghostty-vt--encode-key term "m" nil nil nil)))))
