/* Copyright (c) 2015 Ryan Castellucci, All Rights Reserved */
#include <time.h>
#include <unistd.h>
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>

#include <openssl/sha.h>
#include <openssl/ripemd.h>
#include <openssl/obj_mac.h>

#include <arpa/inet.h> /* for ntohl/htonl */

#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>

#include "secp256k1/include/secp256k1.h"

#include "hex.h"
#include "bloom.h"
#include "hash160.h"

#include "brainv2.h"
#include "warpwallet.h"
#include "brainwalletio.h"
//Magic
#include <mpi.h>
static int brainflayer_is_init = 0;

static unsigned char hash256[SHA256_DIGEST_LENGTH];
static hash160_t hash160_tmp;
static hash160_t hash160_compr;
static hash160_t hash160_uncmp;
static unsigned char *mem;

static unsigned char *bloom = NULL;

static unsigned char unhexed[4096];

static SHA256_CTX *sha256_ctx;
static RIPEMD160_CTX *ripemd160_ctx;

#define bail(code, ...) \
do { \
  fprintf(stderr, __VA_ARGS__); \
  exit(code); \
} while (0)

uint64_t time_1, time_2;
int64_t time_delta;

uint64_t getns() {
	uint64_t ns;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	ns = ts.tv_nsec;
	ns += ts.tv_sec * 1000000000ULL;
	return ns;
}

inline static int priv2hash160(unsigned char *priv) {
	/* only initialize stuff once */
	if (!brainflayer_is_init) {
		/* initialize buffers */
		mem = malloc(4096);

		/* initialize hashs */
		sha256_ctx = malloc(sizeof(*sha256_ctx));
		ripemd160_ctx = malloc(sizeof(*ripemd160_ctx));

		/* set the flag */
		brainflayer_is_init = 1;
	}

	unsigned char *pub_chr = mem;
	int pub_chr_sz;

	secp256k1_ecdsa_pubkey_create(pub_chr, &pub_chr_sz, priv, 0);

#if 0
	i = 0;
	for (i = 0; i < pub_chr_sz; i++) {
		printf("%02x", pub_chr[i]);
	}
	printf("\n");
#endif

	/* compute hash160 for uncompressed public key */
	/* sha256(pub) */
	SHA256_Init(sha256_ctx);
	SHA256_Update(sha256_ctx, pub_chr, pub_chr_sz);
	SHA256_Final(hash256, sha256_ctx);
	/* ripemd160(sha256(pub)) */
	RIPEMD160_Init(ripemd160_ctx);
	RIPEMD160_Update(ripemd160_ctx, hash256, SHA256_DIGEST_LENGTH);
	RIPEMD160_Final(hash160_tmp.uc, ripemd160_ctx);

	/* save result to global struct */
	memcpy(hash160_uncmp.uc, hash160_tmp.uc, 20);

	/* quick and dirty public key compression */
	pub_chr[0] = 0x02 | (pub_chr[64] & 0x01);

	/* compute hash160 for compressed public key */
	/* sha256(pub) */
	SHA256_Init(sha256_ctx);
	SHA256_Update(sha256_ctx, pub_chr, 33);
	SHA256_Final(hash256, sha256_ctx);
	/* ripemd160(sha256(pub)) */
	RIPEMD160_Init(ripemd160_ctx);
	RIPEMD160_Update(ripemd160_ctx, hash256, SHA256_DIGEST_LENGTH);
	RIPEMD160_Final(hash160_tmp.uc, ripemd160_ctx);

	/* save result to global struct */
	memcpy(hash160_compr.uc, hash160_tmp.uc, 20);

	return 0;
}

static int pass2hash160(unsigned char *pass, size_t pass_sz) {
	/* only initialize stuff once */
	if (!brainflayer_is_init) {
		/* initialize buffers */
		mem = malloc(4096);

		/* initialize hashs */
		sha256_ctx = malloc(sizeof(*sha256_ctx));
		ripemd160_ctx = malloc(sizeof(*ripemd160_ctx));

		/* set the flag */
		brainflayer_is_init = 1;
	}

	/* privkey = sha256(passphrase) */
	SHA256_Init(sha256_ctx);
	SHA256_Update(sha256_ctx, pass, pass_sz);
	SHA256_Final(hash256, sha256_ctx);

	return priv2hash160(hash256);
}

static int hexpass2hash160(unsigned char *hpass, size_t hpass_sz) {
	return pass2hash160(unhex(hpass, hpass_sz, unhexed, sizeof(unhexed)),
			hpass_sz >> 1);
}

static int hexpriv2hash160(unsigned char *hpriv, size_t hpriv_sz) {
	return priv2hash160(unhex(hpriv, hpriv_sz, unhexed, sizeof(unhexed)));
}

static unsigned char *kdfsalt;
static size_t kdfsalt_sz;

static int warppass2hash160(unsigned char *pass, size_t pass_sz) {
	int ret;
	if ((ret = warpwallet(pass, pass_sz, kdfsalt, kdfsalt_sz, hash256)) != 0)
		return ret;
	pass[pass_sz] = 0;
	return priv2hash160(hash256);
}

static int bwiopass2hash160(unsigned char *pass, size_t pass_sz) {
	int ret;
	if ((ret = brainwalletio(pass, pass_sz, kdfsalt, kdfsalt_sz, hash256)) != 0)
		return ret;
	pass[pass_sz] = 0;
	return priv2hash160(hash256);
}

static int brainv2pass2hash160(unsigned char *pass, size_t pass_sz) {
	unsigned char hexout[33];
	int ret;
	if ((ret = brainv2(pass, pass_sz, kdfsalt, kdfsalt_sz, hexout)) != 0)
		return ret;
	pass[pass_sz] = 0;
	return pass2hash160(hexout, sizeof(hexout) - 1);
}

static unsigned char *kdfpass;
static size_t kdfpass_sz;

static int warpsalt2hash160(unsigned char *salt, size_t salt_sz) {
	int ret;
	if ((ret = warpwallet(kdfpass, kdfpass_sz, salt, salt_sz, hash256)) != 0)
		return ret;
	salt[salt_sz] = 0;
	return priv2hash160(hash256);
}

static int bwiosalt2hash160(unsigned char *salt, size_t salt_sz) {
	int ret;
	if ((ret = brainwalletio(kdfpass, kdfpass_sz, salt, salt_sz, hash256)) != 0)
		return ret;
	salt[salt_sz] = 0;
	return priv2hash160(hash256);
}

static int brainv2salt2hash160(unsigned char *salt, size_t salt_sz) {
	unsigned char hexout[33];
	int ret;
	if ((ret = brainv2(kdfpass, kdfpass_sz, salt, salt_sz, hexout)) != 0)
		return ret;
	salt[salt_sz] = 0;
	return pass2hash160(hexout, sizeof(hexout) - 1);
}

// function pointer
static int (*input2hash160)(unsigned char *, size_t);

inline static void fprintresult(FILE *f, hash160_t *hash,
		unsigned char compressed, unsigned char *type, unsigned char *input) {
	fprintf(f, "%08x%08x%08x%08x%08x:%c:%s:%s\n", ntohl(hash->ul[0]),
			ntohl(hash->ul[1]), ntohl(hash->ul[2]), ntohl(hash->ul[3]),
			ntohl(hash->ul[4]), compressed, type, input);
}

void usage(unsigned char *name) {
	printf(
			"Usage: %s [OPTION]...\n\n\
 -a                          open output file in append mode\n\
 -b FILE                     check for matches against bloom filter FILE\n\
 -i FILE                     read from FILE instead of stdin\n\
 -o FILE                     write to FILE instead of stdout\n\
 -t TYPE                     inputs are TYPE - supported types:\n\
                             str (default) - classic brainwallet passphrases\n\
                             hex  - classic brainwallets (hex encoded)\n\
                             priv - hex encoded private keys\n\
                             warp - WarpWallet (supports -s or -p)\n\
                             bwio - brainwallet.io (supports -s or -p)\n\
                             bv2  - brainv2 (supports -s or -p) VERY SLOW\n\
 -s SALT                     use SALT for salted input types (default: none)\n\
 -p PASSPHRASE               use PASSPHRASE for salted input types, inputs\n\
                             will be treated as salts\n\
 -h                          show this help\n",
			name);
//q, --quiet                 suppress non-error messages
	exit(1);
}
void readlines(MPI_File *in, const int rank, const int size, const int overlap, char ***lines, int *nlines) {
	/*@see: http://stackoverflow.com/a/13328819/2521647 */
	MPI_Offset filesize;
	MPI_Offset localsize;
	MPI_Offset start;
	MPI_Offset end;
	char *chunk;

	/* figure out who reads what */

	MPI_File_get_size(*in, &filesize);
	localsize = filesize / size;
	start = rank * localsize;
	end = start + localsize - 1;

	/* add overlap to the end of everyone's chunk... */
	end += overlap;

	/* except the last processor, of course */
	if (rank == size - 1)
		end = filesize;

	localsize = end - start + 1;

	/* allocate memory */
	chunk = malloc((localsize + 1) * sizeof(char));

	/* everyone reads in their part */
	MPI_File_read_at_all(*in, start, chunk, localsize, MPI_CHAR,
			MPI_STATUS_IGNORE);
	chunk[localsize] = '\0';

	/*
	 * everyone calculate what their start and end *really* are by going
	 * from the first newline after start to the first newline after the
	 * overlap region starts (eg, after end - overlap + 1)
	 */

	int locstart = 0, locend = localsize;
	if (rank != 0) {
		while (chunk[locstart] != '\n')
			locstart++;
		locstart++;
	}
	if (rank != size - 1) {
		locend -= overlap;
		while (chunk[locend] != '\n')
			locend++;
	}
	localsize = locend - locstart + 1;

	/* Now let's copy our actual data over into a new array, with no overlaps */
	char *data = (char *) malloc((localsize + 1) * sizeof(char));
	memcpy(data, &(chunk[locstart]), localsize);
	data[localsize] = '\0';
	free(chunk);

	/* Now we'll count the number of lines */
	*nlines = 0;
	for (int i = 0; i < localsize; i++)
		if (data[i] == '\n')
			(*nlines)++;

	/* Now the array lines will point into the data array at the start of each line */
	/* assuming nlines > 1 */
	*lines = (char **) malloc((*nlines) * sizeof(char *));
	(*lines)[0] = strtok(data, "\n");
	for (int i = 1; i < (*nlines); i++)
		(*lines)[i] = strtok(NULL, "\n");

	return;
}
int main(int argc, char **argv) {
	int rank;
	int size;

	MPI_Init(&argc, &argv);
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &size);
	printf("Moin. I'm process %d of %d processes.\n", rank, size);
	//FILE *ifile = stdin;
	//MPI_File ifile = stdin;
	MPI_File ifile;
	FILE *ofile = stdout;

	int c, spok = 0, aopt = 0;
	unsigned char *bopt = NULL, *iopt = NULL, *oopt = NULL;
	unsigned char *topt = NULL, *sopt = NULL, *popt = NULL;
	unsigned char *Wopt = NULL;

	while ((c = getopt(argc, argv, "ab:hi:o:p:s:t:W:")) != -1) {
		switch (c) {
		case 'a':
			aopt = 1; // open output file in append mode
			break;
		case 'b':
			bopt = optarg; // bloom filter file
			break;
		case 'i':
			iopt = optarg; // input file
			break;
		case 'o':
			oopt = optarg; // output file
			break;
		case 's':
			sopt = optarg; // salt
			break;
		case 'p':
			popt = optarg; // passphrase
			break;
		case 't':
			topt = optarg; // type of input
			break;
		case 'W':
			Wopt = optarg;
			break;
		case 'h':
			// show help
			usage(argv[0]);
			return 0;
		case '?':
			// show error
			return 1;
		default:
			// should never be reached...
			printf("got option '%c' (%d)\n", c, c);
			return 1;
		}
	}

	if (optind < argc) {
		if (optind == 1 && argc == 2) {
			// older versions of brainflayer had the bloom filter file as a
			// single optional argument, this keeps compatibility with that
			bopt = argv[1];
		} else {
			fprintf(stderr, "Invalid arguments:\n");
			while (optind < argc) {
				fprintf(stderr, "    '%s'\n", argv[optind++]);
			}
			exit(1);
		}
	}

	if (topt != NULL) {
		if (strcmp(topt, "str") == 0) {
			input2hash160 = &pass2hash160;
		} else if (strcmp(topt, "hex") == 0) {
			input2hash160 = &hexpass2hash160;
		} else if (strcmp(topt, "priv") == 0) {
			input2hash160 = &hexpriv2hash160;
		} else if (strcmp(topt, "warp") == 0) {
			spok = 1;
			input2hash160 = popt ? &warpsalt2hash160 : &warppass2hash160;
		} else if (strcmp(topt, "bwio") == 0) {
			spok = 1;
			if (popt && strcmp(popt, "hello world") == 0
					&& (Wopt == NULL
							|| strcmp(Wopt, "NEVER_TELL_ME_THE_ODDS") != 0)) {
				// https://www.youtube.com/watch?v=NHWjlCaIrQo
				for (c = 0; c < 100; ++c)
					fprintf(stderr, "\n"); // try not to clobber scrollback
				fprintf(stderr, "\033[2J\033[0;0H\033[0;0f"); // clear terminal and send cursor to top left
				fflush(stderr);
				sleep(1);
				fprintf(stderr, "A STRANGE GAME.\n");
				sleep(2);
				fprintf(stderr, "THE ONLY WINNING MOVE IS NOT TO PLAY.\n");
				sleep(2);
				fprintf(stderr,
						"\n"
								"So, you're looking for that sweet, sweet 0.5BTC bounty? Brainflayer's\n"
								"cracking speed against brainwallet.io had been communicated to them before\n"
								"the challange was created. It is likely that the salt was chosen to be\n"
								"infeasible to crack in the given timeframe by a significant margin. I advise\n"
								"against trying it - it's probably a waste time and money. If you want to do it\n"
								"anyway, run with `-W NEVER_TELL_ME_THE_ODDS`.\n");
				sleep(2);
				fprintf(stderr,
						"\nAs for me, I have better things to do with my CPU cycles.\n");
				sleep(3);
				bail(83, "CONNECTION TERMINATED\n");
			}
			input2hash160 = popt ? &bwiosalt2hash160 : &bwiopass2hash160;
		} else if (strcmp(topt, "bv2") == 0) {
			spok = 1;
			input2hash160 = popt ? &brainv2salt2hash160 : &brainv2pass2hash160;
		} else {
			bail(1, "Unknown input type '%s'.\n", topt);
		}
	} else {
		topt = "str";
		input2hash160 = &pass2hash160;
	}

	if (spok) {
		if (sopt && popt) {
			bail(1, "Cannot specify both a salt and a passphrase\n");
		}
		if (popt) {
			kdfpass = popt;
			kdfpass_sz = strlen(popt);
		} else {
			if (sopt) {
				kdfsalt = sopt;
				kdfsalt_sz = strlen(kdfsalt);
			} else {
				kdfsalt = malloc(0);
				kdfsalt_sz = 0;
			}
		}
	} else {
		if (popt) {
			bail(1,
					"Specifying a passphrase not supported with input type '%s'\n",
					topt);
		} else if (sopt) {
			bail(1,
					"Specifying a salt not supported with this input type '%s'\n",
					topt);
		}
	}

	if (bopt) {
		if ((bloom = bloom_open(bopt)) == NULL) {
			bail(1, "failed to open bloom filter.\n");
		}
	}

	if (iopt) {
		if (MPI_File_open(MPI_COMM_WORLD, iopt, MPI_MODE_RDONLY, MPI_INFO_NULL,
				&ifile)) {
			bail(1, "failed to open '%s' for reading: %s\n", iopt,
					strerror(errno));
		}
	}

	if (oopt) {
		if ((ofile = fopen(oopt, (aopt ? "a" : "w"))) == NULL) {
			bail(1, "failed to open '%s' for writing: %s\n", oopt,
					strerror(errno));
		}
	}

	/* use line buffered output */
	setvbuf(ofile, NULL, _IOLBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	secp256k1_start();
	const int overlap = 100;
	char **lines;
	int nlines;
	readlines(&ifile, rank, size, overlap, &lines, &nlines);
	fprintf(ofile, "----Welcome %d! %d Lines for you.----\n", rank, nlines);
	int index = 0;
	time_t start, end;
	double length;
	time(&start);
	for (int i = 0; i < nlines - 1; i++) {
		++index;
		input2hash160(lines[i], strlen(lines[i]));
		if (bloom) {
			if (bloom_chk_hash160(bloom, hash160_uncmp.ul)) {
				fprintresult(ofile, &hash160_uncmp, 'u', topt, lines[i]);
			}
			if (bloom_chk_hash160(bloom, hash160_compr.ul)) {
				fprintresult(ofile, &hash160_compr, 'c', topt, lines[i]);
			}
		} else {
			fprintresult(ofile, &hash160_uncmp, 'u', topt, lines[i]);
			fprintresult(ofile, &hash160_compr, 'c', topt, lines[i]);
		}
	}
	time(&end);
	length = difftime(end, start);
	double perSecond = index / length;
	fprintf(ofile, "----Process: %d, Lines: %d, speed: %.0f/sec!----\n", rank, index, perSecond);
	secp256k1_stop();
	MPI_File_close(&ifile);
	MPI_Finalize();
	return 0;
}

/*  vim: set ts=2 sw=2 et ai si: */
