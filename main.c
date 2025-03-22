#include <stdio.h>
#include <pocketsphinx.h>
#include <sphinxbase/cmd_ln.h>
#include <stdlib.h>
#include <math.h>


int main(int argc, char* argv[]) {
    // Konfiguracija prepoznavanja
    ps_decoder_t *ps;
    cmd_ln_t *config;
    FILE *fh;
    char const *hyp;
    
    // Ukoliko nisu prosledjeni argumenti, ispisujemo poruku i zavrsavamo program

    // Inicijalizacija PocketSphinx-a
    if(argc < 2){
     printf("nedovoljno argumenata");
     return 1;
    }
    config = cmd_ln_init(NULL, ps_args(), TRUE,
                 "-hmm", "/usr/local/include/pocketsphinx/model/en-us/en-us",
                 "-lm", "/usr/local/include/pocketsphinx/model/en-us/recnik.lm.bin",
                 "-dict", "/usr/local/include/pocketsphinx/model/en-us/recnik.dict",
                 "-samprate", "48000",
                 NULL);
    if (config == NULL) {
        fprintf(stderr, "Greska pri inicijalizaciji PocketSphinx-a.\n");
        return 1;
    }

    ps = ps_init(config);
    if (ps == NULL) {
        fprintf(stderr, "Greska pri inicijalizaciji PocketSphinx-a.\n");
        return 1;
    }
    // Otvaranje audio fajla
    fh = fopen(argv[1], "rb");
    if (fh == NULL) {
        fprintf(stderr, "Greska pri otvaranju audio fajla.\n");
        return 1;
    }

    // Konverzija audio formata u PCM
    if (ps_start_utt(ps) < 0) {
        fprintf(stderr, "Greska pri pocetku procesiranja zvuka.\n");
        return 1;
    }

    // Procesiranje audio fajla
    while (!feof(fh)) {
        int16 buf[512];
        size_t nsamp;
        nsamp = fread(buf, 2, 512, fh);
        ps_process_raw(ps, buf, nsamp, FALSE, FALSE);//kriticna funkcija
    }

    // Zavrsetak prepoznavanja
    ps_end_utt(ps);

    // Ispis rezultata
    hyp = ps_get_hyp(ps, NULL);
    if (hyp != NULL) {
        printf("Rekao/la si: %s\n", hyp);
    } else {
        printf("Greska u prepoznavanju.\n");
    }
    // Oslobadjanje resursa
    fclose(fh);
    ps_free(ps);
    cmd_ln_free_r(config);
 
    return 0;
}
