# NFS Sync Tool

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)
![License](https://img.shields.io/badge/license-Apache%202.0-green.svg)
![Tests](https://img.shields.io/badge/tests-68%20passing-brightgreen.svg)

> **A multithreaded, network-based directory synchronization tool in C++17.**
> A central manager with a bounded worker-thread pool orchestrates `LIST → PULL → PUSH`
> file transfers between remote agents over TCP, with a live admin console, graceful
> shutdown, per-file audit logging, and a 68-case automated test suite (valgrind-clean).
> Documentation below is in Greek.

Κατανεμημένος συγχρονισμός αρχείων μεταξύ απομακρυσμένων καταλόγων, πάνω από TCP.
Ένας κεντρικός **manager** με pool από worker threads ορχηστρώνει μεταφορές
`LIST → PULL → PUSH` ανάμεσα σε **clients** που εκθέτουν τοπικούς καταλόγους, ενώ ένα
**console** επιτρέπει διαχείριση σε πραγματικό χρόνο χωρίς επανεκκίνηση.

Υλοποίηση σε C++17 με POSIX sockets, threads και condition variables. Χωρίς
εξωτερικές εξαρτήσεις, χωρίς κλήσεις σε `scp` / `rsync` / shell.

> Εργασία 2 – Συστήματα Προγραμματισμού, Τμήμα Πληροφορικής & Τηλεπικοινωνιών ΕΚΠΑ.
---

## Περιεχόμενα

- [Αρχιτεκτονική](#αρχιτεκτονική)
- [Μεταγλώττιση](#μεταγλώττιση)
- [Γρήγορη εκκίνηση](#γρήγορη-εκκίνηση)
- [Διεπαφή γραμμής εντολών](#διεπαφή-γραμμής-εντολών)
- [Αρχείο ρυθμίσεων](#αρχείο-ρυθμίσεων)
- [Εντολές console](#εντολές-console)
- [Πρωτόκολλο επικοινωνίας](#πρωτόκολλο-επικοινωνίας)
- [Καταγραφή (logging)](#καταγραφή-logging)
- [Μοντέλο συγχρονισμού νημάτων](#μοντέλο-συγχρονισμού-νημάτων)
- [Διαχείριση σφαλμάτων](#διαχείριση-σφαλμάτων)
- [Έλεγχος ορθότητας](#έλεγχος-ορθότητας)
- [Δομή του κώδικα](#δομή-του-κώδικα)
- [Enterprise usage](#enterprise-usage)
- [Όρια και μη-στόχοι](#όρια-και-μη-στόχοι)
- [Αντιμετώπιση προβλημάτων](#αντιμετώπιση-προβλημάτων)
- [License](#license)

---

## Αρχιτεκτονική

```
                       ┌──────────────────────────┐
   console commands    │        nfs_manager       │
   ──────────────────► │                          │
   add / cancel /      │  ┌────────────────────┐  │
   shutdown            │  │   BoundedBuffer    │  │   ένα task ανά αρχείο
                       │  │  producer/consumer │  │
                       │  │   + back-pressure  │  │
                       │  └─────────┬──────────┘  │
                       │            │             │
                       │   ┌────────▼────────┐    │
                       │   │ worker thread 1 │    │
                       │   │ worker thread 2 │    │
                       │   │      ...  N     │    │
                       │   └────┬───────┬────┘    │
                       └────────│───────│─────────┘
                        PULL    │       │   PUSH
                    ┌───────────▼─┐   ┌─▼───────────┐
                    │ nfs_client  │   │ nfs_client  │
                    │  (source)   │   │  (target)   │
                    │ /src_dir    │   │ /tgt_dir    │
                    └─────────────┘   └─────────────┘
```

Ροή μιας μεταφοράς:

1. Το main thread του manager διαβάζει το config και στέλνει `LIST` σε κάθε source client.
2. Για κάθε όνομα αρχείου που επιστρέφεται δημιουργείται ένα `SyncTask` και μπαίνει
   στον `BoundedBuffer`. Αν ο buffer είναι γεμάτος, ο producer μπλοκάρει (back-pressure).
3. Κάθε worker thread αφαιρεί ένα task, κάνει `PULL` το αρχείο από τον source client
   στη μνήμη και το γράφει με `PUSH` στον target client, σε chunks των 4 KB.
4. Κάθε φάση καταγράφεται στο manager log με αποτέλεσμα `SUCCESS` ή `ERROR`.
5. Ένα ξεχωριστό thread ακούει για εντολές console· κάθε εντολή εξυπηρετείται σε δικό
   της thread, ώστε ο συγχρονισμός να μη σταματά ποτέ για διαχειριστικές ενέργειες.

## Μεταγλώττιση

Απαιτήσεις: `g++` με υποστήριξη C++17, GNU Make, Linux (δοκιμασμένο σε Ubuntu 22.04 / WSL2).

```bash
make clean && make all
```

Παράγει τρία εκτελέσιμα: `nfs_manager`, `nfs_client`, `nfs_console`.
Η μεταγλώττιση γίνεται με `-std=c++17 -Wall -Wextra -pthread` και δεν παράγει προειδοποιήσεις.

## Γρήγορη εκκίνηση

Τέσσερα terminals στο ίδιο μηχάνημα:

```bash
# 1. Προετοιμασία
mkdir -p /tmp/demo/src /tmp/demo/tgt
echo "Hello from syspro2" > /tmp/demo/src/file1.txt
cat > /tmp/demo/nfs.cfg <<'EOF'
/tmp/demo/src@127.0.0.1:5556 /tmp/demo/tgt@127.0.0.1:5557
EOF

# 2. terminal A - client που εκθέτει τον source κατάλογο
./nfs_client -p 5556 -l source_client.log

# 3. terminal B - client που εκθέτει τον target κατάλογο
./nfs_client -p 5557 -l target_client.log

# 4. terminal C - manager: 5 workers, buffer 7 θέσεων, console port 5555
./nfs_manager -l manager.log -c /tmp/demo/nfs.cfg -n 5 -p 5555 -b 7

# 5. terminal D - console
./nfs_console -l console.log -h 127.0.0.1 -p 5555
> add /tmp/demo/src2@127.0.0.1:5556 /tmp/demo/tgt2@127.0.0.1:5557
> cancel /tmp/demo/src
> shutdown
```

Σε πολλαπλά μηχανήματα, το μόνο που αλλάζει είναι τα hostnames στο config file και
στο `-h` του console.

## Διεπαφή γραμμής εντολών

### `nfs_manager`

```
./nfs_manager -l <manager_logfile> -c <config_file> -n <worker_limit> -p <port_number> -b <bufferSize>
```

| Όρισμα | Περιγραφή |
|---|---|
| `-l` | Αρχείο καταγραφής του manager. Δημιουργείται/μηδενίζεται στην εκκίνηση. |
| `-c` | Αρχείο ρυθμίσεων με τα ζεύγη source → target. |
| `-n` | Πλήθος worker threads στο pool. |
| `-p` | Θύρα στην οποία ακούει για συνδέσεις από το `nfs_console`. |
| `-b` | Χωρητικότητα του bounded buffer σε tasks (αρχεία). |

Όλα τα ορίσματα είναι υποχρεωτικά. Μη έγκυρη τιμή ή ελλιπές όρισμα τερματίζει με
κωδικό εξόδου `1` και μήνυμα χρήσης.

### `nfs_client`

```
./nfs_client -p <port_number> [-l <client_logfile>]
```

| Όρισμα | Περιγραφή |
|---|---|
| `-p` | Θύρα στην οποία δέχεται εντολές από τον manager. |
| `-l` | *(προαιρετικό)* Αρχείο καταγραφής. Προεπιλογή: `nfs_client_<port>.log`. |

Ο client εξυπηρετεί κάθε σύνδεση του manager σε ξεχωριστό thread, οπότε ένας client
μπορεί να λειτουργεί ταυτόχρονα ως source και ως target για πολλαπλούς καταλόγους.

### `nfs_console`

```
./nfs_console -l <console-logfile> -h <host_IP> -p <host_port>
```

| Όρισμα | Περιγραφή |
|---|---|
| `-l` | Αρχείο καταγραφής των εντολών. |
| `-h` | Διεύθυνση/hostname του manager. |
| `-p` | Θύρα εντολών του manager (το `-p` του manager). |

Το console ανοίγει νέα σύνδεση ανά εντολή, τυπώνει όλες τις γραμμές απάντησης και
τερματίζει μετά από `shutdown` ή EOF στην είσοδο. Δέχεται και ροή εντολών από pipe,
οπότε μπορεί να χρησιμοποιηθεί σε scripts:

```bash
printf 'add /a@h1:5556 /b@h2:5557\nshutdown\n' | ./nfs_console -l c.log -h mgr -p 5555
```

## Αρχείο ρυθμίσεων

Μία γραμμή ανά ζεύγος συγχρονισμού:

```
/source_dir@source_host:source_port /target_dir@target_host:target_port
```

Παράδειγμα:

```
# production assets
/srv/assets@10.0.1.10:5556      /var/www/assets@10.0.2.20:5557
/srv/reports@10.0.1.10:5556     /mnt/archive/reports@10.0.3.30:5557
```

- Τα μονοπάτια ερμηνεύονται **στο μηχάνημα του αντίστοιχου client**, γι' αυτό
  συνιστώνται απόλυτα μονοπάτια.
- Κενές γραμμές και γραμμές που ξεκινούν με `#` αγνοούνται.
- Υποστηρίζονται αρχεία με τερματισμό γραμμής CRLF.
- Μη έγκυρη γραμμή ή θύρα εκτός του `1–65535` τερματίζει τον manager με μήνυμα σφάλματος.

## Εντολές console

| Εντολή | Περιγραφή |
|---|---|
| `add <source_dir@host:port> <target_dir@host:port>` | Καταχωρεί νέο ζεύγος, κάνει `LIST` και βάζει αμέσως τα αρχεία στην ουρά. |
| `cancel <source_dir>` | Σταματά τον συγχρονισμό του καταλόγου και ακυρώνει τα tasks του που περιμένουν στην ουρά. Δέχεται και τη μορφή `<source_dir@host:port>`. |
| `shutdown` | Τερματίζει ομαλά τον manager. |

Απαντήσεις (εμφανίζονται στο console και καταγράφονται στο manager log):

```
[2025-02-10 10:00:01] Added file: /dir1/file1@1.2.3.4:8080 -> /dir2/file1@4.5.6.7:8090
[2025-02-10 10:00:05] Already in queue: /dir1@1.2.3.4:8080
[2025-02-10 10:23:01] Synchronization stopped for /dir1@1.2.3.4:8080
[2025-02-10 10:23:05] Directory not being synchronized: /dir9
[2025-02-10 10:23:02] Shutting down manager...
[2025-02-10 10:23:02] Waiting for all active workers to finish.
[2025-02-10 10:23:03] Processing remaining queued tasks.
[2025-02-10 10:24:01] Manager shutdown complete.
```

Σημασιολογία του `shutdown`: ο buffer παύει να δέχεται νέα tasks, τα ήδη ενεργά
ολοκληρώνονται, η ουρά αδειάζει κανονικά και μόνο τότε τερματίζουν τα worker threads
και η διεργασία. Καμία μεταφορά δεν διακόπτεται στη μέση.

## Πρωτόκολλο επικοινωνίας

Κειμενικές εντολές με δυαδικό ωφέλιμο φορτίο, πάνω από TCP. Ο manager είναι πάντα
ο client της σύνδεσης· ο `nfs_client` δεν ξεκινά ποτέ σύνδεση μόνος του.

### LIST

```
→  LIST <source_dir>\n
←  file1.txt\n
   file2.txt\n
   .\n
```

Επιστρέφονται μόνο κανονικά αρχεία· υποκατάλογοι και ειδικά αρχεία παραλείπονται.
Η τελεία σε δική της γραμμή τερματίζει τη λίστα.

### PULL

```
→  PULL <source_dir>/<file>\n
←  <filesize><space><data>
```

Σε σφάλμα ο client απαντά `-1 <error message>\n` (π.χ. `-1 Permission denied`).
Το `<filesize>` είναι δεκαδικός αριθμός bytes· ακολουθούν **ακριβώς** τόσα bytes.
Ο manager διαβάζει την κεφαλίδα byte-προς-byte, ώστε να μην καταναλώσει ποτέ κατά
λάθος δυαδικά δεδομένα.

### PUSH

```
→  PUSH <target_dir>/<file> -1\n          δημιουργία / μηδενισμός αρχείου
→  PUSH <target_dir>/<file> <n> <n bytes> ένα chunk δεδομένων (n ≤ 4096)
→  PUSH <target_dir>/<file> 0\n           τέλος αρχείου, κλείσιμο descriptor
```

Ο target client δημιουργεί αυτόματα τους ενδιάμεσους καταλόγους που λείπουν.

## Καταγραφή (logging)

Και τα τρία εκτελέσιμα γράφουν thread-safe logs με χρονοσφραγίδα `YYYY-MM-DD HH:MM:SS`
και flush μετά από κάθε εγγραφή, ώστε τα αρχεία να είναι χρήσιμα και σε `tail -f`.

**Manager – μία γραμμή ανά μεταφορά:**

```
[TIMESTAMP] [SOURCE_DIR] [TARGET_DIR] [THREAD_PID] [OPERATION] [RESULT] [DETAILS]
```

```
[2025-02-10 10:00:01] [/dir1/file1@1.2.3.4:8080] [/dir2/file1@4.5.6.7:8090] [1234] [PULL] [SUCCESS] [10 bytes pulled]
[2025-02-10 10:00:02] [/dir1/file1@1.2.3.4:8080] [/dir2/file1@4.5.6.7:8090] [1234] [PUSH] [SUCCESS] [10 bytes pushed]
[2025-02-10 10:00:03] [/dir1/file2@1.2.3.4:8080] [/dir2/file2@4.5.6.7:8090] [1235] [PULL] [ERROR] [File: file2.txt Permission denied]
```

`THREAD_PID` είναι το πραγματικό αναγνωριστικό νήματος του λειτουργικού (`gettid`),
ώστε οι γραμμές να αντιστοιχίζονται με ό,τι δείχνουν τα `top -H`, `ps -L` ή `perf`.

**Console – μία γραμμή ανά εντολή:**

```
[2025-02-10 10:00:01] Command add /dir1@1.2.3.4:8080 -> /dir2@4.5.6.7:8090
[2025-02-10 10:23:01] Command cancel /dir1
[2025-02-10 10:23:01] Command shutdown
```

**Client – μία γραμμή ανά εντολή που εξυπηρετήθηκε**, με το μέγεθος των δεδομένων και
την αιτία τυχόν αποτυχίας.

## Μοντέλο συγχρονισμού νημάτων

| Μηχανισμός | Ρόλος |
|---|---|
| `BoundedBuffer` | Ουρά παραγωγού-καταναλωτή με δύο condition variables (`cv_full_`, `cv_empty_`). Το άνω όριο περιορίζει τη χρήση μνήμης και επιβάλλει back-pressure στο LIST. |
| `all_syncs_mtx` | Προστατεύει τον πίνακα ενεργών συγχρονισμών (κατάσταση, μετρητές σφαλμάτων, χρόνος τελευταίου συγχρονισμού). |
| `log_mutex` | Σειριοποιεί τις εγγραφές στα log αρχεία. |
| `std::once_flag` | Εγγυάται ότι τα worker threads γίνονται join ακριβώς μία φορά, είτε ο τερματισμός ξεκινήσει από το console είτε από το main thread. |
| `shutdown()` στο listening socket | Ξυπνά το μπλοκαρισμένο `accept()` ώστε το command thread να τερματίσει ντετερμινιστικά, χωρίς busy-wait ή signals. |

Ο πίνακας συγχρονισμών αντιγράφεται υπό κλείδωμα και η δικτυακή επικοινωνία γίνεται
εκτός κρίσιμης περιοχής, ώστε μια αργή ή μη αποκρίσιμη σύνδεση να μη μπλοκάρει ποτέ
τις εντολές του console.

## Διαχείριση σφαλμάτων

- Αποτυχία σύνδεσης σε source ή target καταγράφεται ως `[ERROR]`, αυξάνει τον μετρητή
  σφαλμάτων του ζεύγους και **δεν** ρίχνει τον manager· το επόμενο task συνεχίζει κανονικά.
- Σφάλματα από την πλευρά του client (`-1 <reason>`) μεταφέρονται αυτούσια στο log,
  στο πεδίο `DETAILS`.
- Κάθε εντολή του console εκτελείται μέσα σε `try/catch`: συντακτικά λάθη απαντώνται
  ως μήνυμα, ποτέ ως τερματισμός της διεργασίας.
- `SIGPIPE` αγνοείται· διακοπή σύνδεσης εμφανίζεται ως αποτυχία εγγραφής και
  αντιμετωπίζεται τοπικά.
- Αν η θύρα εντολών είναι κατειλημμένη, ο manager το καταγράφει και τερματίζει αντί
  να μείνει σε ασυνεπή κατάσταση.

## Έλεγχος ορθότητας

Το αποθετήριο περιλαμβάνει αυτοματοποιημένη σουίτα ελέγχων:

```bash
./run_tests.sh              # λειτουργικοί έλεγχοι
./run_tests.sh --valgrind   # και έλεγχος διαρροών μνήμης

bash run_tests.sh           # αν χαθεί το execute bit στο checkout
PORT_BASE=8200 ./run_tests.sh   # αν οι προεπιλεγμένες θύρες 7700+ είναι πιασμένες
```

Η σουίτα στήνει πραγματικές διεργασίες σε απομονωμένο προσωρινό κατάλογο και καλύπτει:

| Ενότητα | Τι ελέγχεται |
|---|---|
| Build | Επιτυχής μεταγλώττιση, μηδέν προειδοποιήσεις με `-Wall -Wextra` |
| CLI | Ελλιπή/άκυρα ορίσματα και στα τρία εκτελέσιμα |
| Config | Αρχείο που λείπει, άκυρη γραμμή, θύρα εκτός ορίων, CRLF, σχόλια |
| Πρωτόκολλο | `LIST`, `PULL`, `PULL` σε ανύπαρκτο αρχείο, `PUSH` με chunks, αυτόματη δημιουργία καταλόγων |
| Ακεραιότητα | Αρχεία 0 B, κάτω/πάνω/ακριβώς στο όριο chunk, 3.3 MB δυαδικά — σύγκριση byte προς byte |
| Παραλληλία | Επιβεβαίωση ότι δούλεψαν πολλαπλά worker threads |
| Console | `add`, διπλό `add`, άκυρο `add`, `cancel` με σκέτο μονοπάτι, `cancel` άγνωστου καταλόγου, άγνωστη εντολή |
| Shutdown | Και τα τέσσερα μηνύματα, άδειασμα ουράς, πραγματικός τερματισμός διεργασίας |
| Logs | Επαλήθευση μορφής με regular expressions |
| Back-pressure | 40 αρχεία με `-n 1 -b 1` χωρίς deadlock ή απώλεια δεδομένων |
| Σφάλματα | Πεσμένος target, πεσμένος source, σφάλμα από τον client, μη προσβάσιμος manager, κατειλημμένη θύρα |
| Μνήμη | `valgrind --leak-check=full`: μηδέν definite leaks, μηδέν σφάλματα |

Η σουίτα τερματίζει με κωδικό εξόδου `0` μόνο αν περάσουν όλοι οι έλεγχοι, οπότε
μπορεί να χρησιμοποιηθεί αυτούσια ως βήμα σε CI pipeline.

## Δομή του κώδικα

```
nfs_manager.cpp      Ορχήστρωση: worker pool, LIST/PULL/PUSH, console server, shutdown
nfs_client.cpp       Server ανά κατάλογο: υλοποίηση LIST / PULL / PUSH
nfs_console.cpp      Διαδραστικό CLI προς τον manager
buffer.{h,cpp}       BoundedBuffer: ουρά tasks με back-pressure και graceful drain
config_parser.{h,cpp} Ανάγνωση και επικύρωση του αρχείου ρυθμίσεων
logger.{h,cpp}       Thread-safe logging για manager / client / console
common.h             Κοινές βοηθητικές συναρτήσεις (χρονοσφραγίδες)
Makefile             Στόχοι: all, clean
run_tests.sh         Σουίτα λειτουργικών ελέγχων
LICENSE, NOTICE      Apache License 2.0
```

## Enterprise usage

Η αρχιτεκτονική —κεντρική ορχήστρωση, πράκτορες ανά κόμβο, ουρά με άνω όριο και
πλήρες audit log ανά αρχείο— αντιστοιχεί σε σενάρια που σε παραγωγικά περιβάλλοντα
λύνονται συνήθως με ad-hoc `rsync` σε cron. Ενδεικτικές εφαρμογές:

**1. Διανομή artifacts σε edge κόμβους.**
Μετά από ένα επιτυχές build, το pipeline στέλνει `add /builds/release-42@build01:5556
/opt/app/releases@edge07:5557` στο console. Ο manager διανέμει τα αρχεία σε δεκάδες
edge κόμβους παράλληλα, με το `-n` να ελέγχει πόσοι κόμβοι εξυπηρετούνται ταυτόχρονα
και το `-b` να περιορίζει την κατανάλωση μνήμης. Κάθε αρχείο αφήνει γραμμή audit.

**2. Προώθηση ρυθμίσεων και feature flags.**
Ένας κατάλογος ρυθμίσεων ανά περιβάλλον προωθείται σε κάθε application server.
Το `cancel` απομονώνει άμεσα έναν προβληματικό κόμβο χωρίς επανεκκίνηση του manager
και χωρίς να πειραχτεί το αρχείο ρυθμίσεων.

**3. Συγκέντρωση logs και τηλεμετρίας.**
Με αντίστροφη φορά, οι κόμβοι λειτουργούν ως source και ένας κεντρικός κόμβος ως
target: περιοδική συγκέντρωση αρχείων καταγραφής για ανάλυση, χωρίς να χρειάζεται
πρόσβαση SSH από το κέντρο προς τους κόμβους.

**4. Μεταφορά δεδομένων μεταξύ ζωνών δικτύου.**
Σε περιβάλλοντα με αυστηρό διαχωρισμό (DMZ ↔ εσωτερικό δίκτυο) το firewall χρειάζεται
να επιτρέπει μία μόνο θύρα ανά πράκτορα και ένα μοναδικό, ελεγχόμενο πρωτόκολλο, αντί
για γενική πρόσβαση SSH. Το audit log ανά αρχείο καλύπτει απαιτήσεις ιχνηλασιμότητας
(ποιο αρχείο, από πού, πού, πότε, με τι αποτέλεσμα).

**5. Warm standby / disaster recovery.**
Περιοδική αντιγραφή κρίσιμων καταλόγων (uploads χρηστών, αντίγραφα βάσης, πιστοποιητικά)
σε εφεδρικό κέντρο δεδομένων, με ένα `add` ανά κύκλο από scheduler (cron, systemd timer). Ο μετρητής σφαλμάτων ανά ζεύγος και τα `[ERROR]` στο log
τροφοδοτούν alerting για κόμβο που έμεινε πίσω.

**6. Διανομή μοντέλων και datasets σε ML στόλο.**
Τα βάρη ενός μοντέλου αντιγράφονται σε κόμβους inference πριν την εναλλαγή έκδοσης.
Το `shutdown` που ολοκληρώνει πρώτα τα ενεργά tasks εξασφαλίζει ότι δεν θα μείνει
ποτέ μισογραμμένο αρχείο μοντέλου σε κόμβο παραγωγής.

**7. Staging υλικού σε render / media farms.**
Μεγάλα δυαδικά αρχεία προωθούνται στους κόμβους απόδοσης πριν ξεκινήσει η εργασία,
με το bounded buffer να αποτρέπει τον κορεσμό δικτύου και μνήμης όταν προστίθενται
ταυτόχρονα εκατοντάδες αρχεία.

### Ενσωμάτωση σε λειτουργικό περιβάλλον

Οι πράκτορες και ο manager είναι απλές, μακρόβιες διεργασίες χωρίς εξαρτήσεις, οπότε
ενσωματώνονται άμεσα σε `systemd`:

```ini
# /etc/systemd/system/nfs-client.service
[Unit]
Description=NFS Sync agent
After=network-online.target

[Service]
ExecStart=/opt/nfs-sync/nfs_client -p 5556 -l /var/log/nfs-sync/client.log
Restart=always
User=nfssync

[Install]
WantedBy=multi-user.target
```

Ο manager μπορεί να οδηγείται από automation, αφού το console δέχεται εντολές από pipe:

```bash
# βήμα ενός CI/CD pipeline
printf 'add %s@%s:5556 %s@%s:5557\n' "$ARTIFACT_DIR" "$BUILD_HOST" "$DEPLOY_DIR" "$EDGE_HOST" \
  | nfs_console -l "$CI_LOG" -h "$MANAGER_HOST" -p 5555
```

### Τι θα απαιτούσε μια παραγωγική εγκατάσταση

Η υλοποίηση είναι ακαδημαϊκή και εστιάζει στη σωστή χρήση sockets, threads και
condition variables. Πριν από πραγματική χρήση θα χρειάζονταν:

- **Ασφάλεια:** TLS ή mTLS στις συνδέσεις, αυθεντικοποίηση του manager προς τους
  πράκτορες και περιορισμός των επιτρεπόμενων καταλόγων ανά πράκτορα.
- **Ακεραιότητα:** checksum (π.χ. SHA-256) ανά αρχείο και επαλήθευση μετά το PUSH,
  με εγγραφή σε προσωρινό αρχείο και ατομική μετονομασία.
- **Επαναληπτικός συγχρονισμός:** ανίχνευση αλλαγών με `inotify` ή σύγκριση
  mtime/μεγέθους, αντί για πλήρη αντιγραφή κάθε φορά.
- **Ανθεκτικότητα:** επαναπροσπάθειες με εκθετική καθυστέρηση, timeouts ανά socket,
  επιμονή της ουράς σε δίσκο ώστε να επιβιώνει επανεκκίνησης.
- **Παρατηρησιμότητα:** εξαγωγή μετρικών (Prometheus), δομημένα logs σε JSON.
- **Κλιμάκωση:** streaming του αρχείου από socket σε socket αντί για πλήρη φόρτωση
  στη μνήμη, ώστε το μέγεθος αρχείου να μην περιορίζεται από τη διαθέσιμη RAM.

## Όρια και μη-στόχοι

- Ο συγχρονισμός είναι **μονόδρομος** και **εφάπαξ** ανά καταχώρηση: τα αρχεία
  αντιγράφονται όταν προστεθεί το ζεύγος, όχι συνεχώς.
- Συγχρονίζονται μόνο τα κανονικά αρχεία του πρώτου επιπέδου· υποκατάλογοι,
  symlinks και δικαιώματα/ιδιοκτησία δεν αντιγράφονται.
- Το κάθε αρχείο φορτώνεται εξ ολοκλήρου στη μνήμη του manager κατά το PULL.
- Δεν υπάρχει κρυπτογράφηση ή αυθεντικοποίηση· προορίζεται για έμπιστο δίκτυο.
- Το `cancel` σταματά μελλοντικούς συγχρονισμούς και ακυρώνει tasks της ουράς· δεν
  διακόπτει μεταφορά που ήδη εκτελείται.

## Αντιμετώπιση προβλημάτων

| Σύμπτωμα | Αιτία / λύση |
|---|---|
| `Error connecting to source ...: Connection refused` | Ο source client δεν τρέχει στη θύρα του config. Έλεγχος: `ss -tlnp \| grep 5556` |
| Το log δείχνει `[PULL] [SUCCESS]` αλλά όχι `[PUSH]` | Ο target client δεν είναι προσβάσιμος· η γραμμή `[PUSH] [ERROR]` δίνει τη διεύθυνση που απέτυχε. |
| `[PULL] [ERROR] [File: x Permission denied]` | Ο χρήστης που τρέχει τον source client δεν έχει δικαίωμα ανάγνωσης στο αρχείο. |
| `[PUSH] [ERROR]` με `Error opening for PUSH init` στο client log | Ο target κατάλογος δεν είναι εγγράψιμος από τον χρήστη του client. |
| Ο manager τερματίζει αμέσως με `cannot bind command port` | Η θύρα `-p` χρησιμοποιείται ήδη. |
| Τίποτα δεν συγχρονίζεται και το log δείχνει `No syncs configured` | Το config file είναι κενό ή όλες οι γραμμές είναι σχόλια. |
| Διεργασίες που έμειναν από προηγούμενη δοκιμή | `pkill -f nfs_client; pkill -f nfs_manager; pkill -f nfs_console` |

## License

Διανέμεται υπό την άδεια **Apache License 2.0**. Δείτε τα αρχεία [LICENSE](LICENSE)
και [NOTICE](NOTICE).

---

Συγγραφέας: [Andreaslmpr](https://github.com/Andreaslmpr) · C++17 · Linux / WSL2
