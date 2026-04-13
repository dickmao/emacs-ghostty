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
	   do (sleep-for 2)
	   do (prin1 (buffer-string))
	   finally (should (test-ghostty/at-prompt))))

;; (ert-deftest basic ()
;;   (test-ghostty/with-session
;;     (should (eq major-mode 'ghostty-vt-mode))))

(ert-deftest wrap ()
  (test-ghostty/with-session
    (should (= (window-width) ghostty-vt-min-window-width))
    (let* ((x3 (make-string (* 1 (window-width)) ?x)))
      (test-ghostty/run (format "echo %s" x3))
      (save-excursion
	(forward-line -1)
	(let ((pure-text (buffer-substring-no-properties
			  (line-beginning-position) (line-end-position))))
	  (should (equal pure-text x3))))
      (call-interactively #'ghostty-vt-copy-mode)
      ;; (should (equal (buffer-substring-no-properties
      ;; 		      (line-beginning-position) (point))
      ;; 		     (concat x3 test-ghostty/prompt)))
      )))

;; (ert-deftest yank-pop ()
;;   (test-ghostty/with-session
;;     (should (zerop (length kill-ring)))
;;     (should (test-ghostty/at-prompt))
;;     (test-ghostty/run (format "echo the quick brown fox"))
;;     (call-interactively #'ghostty-vt-copy-mode)
;;     (re-search-backward (regexp-quote "brown"))
;;     (copy-region-as-kill (point) (+ (point) (length "brown")))
;;     (re-search-backward (regexp-quote "quick"))
;;     (copy-region-as-kill (point) (+ (point) (length "quick")))
;;     (call-interactively #'ghostty-vt-copy-mode)
;;     (should (test-ghostty/at-prompt))
;;     ;; i don't know why save-excursion does not work
;;     ;; maybe it's a paste thing
;;     (let ((start (ghostty-vt-reset-cursor-point)))
;;       (call-interactively #'ghostty-vt-yank)
;;       (accept-process-output ghostty-vt--process 0.1)
;;       (goto-char start)
;;       (should (looking-at (regexp-quote "quick")))
;;       (setq last-command 'ghostty-vt-yank)
;;       (call-interactively #'ghostty-vt-yank-pop-dwim)
;;       (accept-process-output ghostty-vt--process 0.1)
;;       (goto-char start)
;;       (should (looking-at (regexp-quote "brown"))))))

(provide 'test-ghostty)
;;; test-ghostty.el ends here
