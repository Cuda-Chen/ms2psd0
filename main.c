#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
//#include <assert.h>

#include "fftw3.h"

#include "autocorr.h"
#include "bandpass_filter.h"
#include "common.h"
#include "cosine_taper.h"
#include "datatype.h"
#include "detrend.h"
#include "fft.h"
#include "freq_response.h"
#include "output2Octave.h"
#include "parse_miniSEED.h"
#include "parse_sacpz.h"
#include "psd.h"
#include "standard_deviation.h"

static void
range (double *array, double sampleRate, int totalSamples)
{
  double delta         = 1. / sampleRate;
  double totalDuration = delta * totalSamples;
  int i;

  for (i = 0; i < totalSamples; i++)
    array[i] = i / totalDuration;
}

static void
usage ()
{
  printf ("Usage: ./ms2psd [f1] [f2] [f3] [f4] [totype] [input] [resp] [output]");
  printf ("\n\nInput parameters:\n");
  printf ("f1, f2, f3, f4: four-corner frequencies (Hz)\n");
  printf ("totype: specify the following numbers for output waveform format:\n");
  printf ("        0: displacement\n");
  printf ("        1: velocity\n");
  printf ("        2: acceleration\n");
  printf ("input: input waveform. Should be miniSEED format\n");
  printf ("resp: response file in SACPZ format\n");
  printf ("output: output waveform in miniSEED format\n");
}

int
main (int argc, char **argv)
{
  char *mseedfile = NULL;
  data_t *data    = NULL;
  double sampleRate;
  uint64_t totalSamples;

  float f1, f2, f3, f4; /* four-corner frequencies */
  int totype;           /* output waveform model, e.g., displacement, velocity, or acceleration */
  char *sacpzfile;
  char *outputFile;
  int rv;

  /* Simple argement parsing */
  if (argc != 9)
  {
    usage ();
    return 1;
  }
  f1         = atof (argv[1]);
  f2         = atof (argv[2]);
  f3         = atoi (argv[3]);
  f4         = atoi (argv[4]);
  totype     = atoi (argv[5]);
  mseedfile  = argv[6];
  sacpzfile  = argv[7];
  outputFile = argv[8];

  /* Get data from input miniSEED file */
  rv = parse_miniSEED (mseedfile, &data, &sampleRate, &totalSamples);
  if (rv != 0)
  {
    return rv;
  }
  if (data == NULL)
  {
    printf ("Input data read unsuccessfully\n");
    return -1;
  }

  /* Method #2 */
  FILE *dataout = fopen ("dataout.txt", "w");
  for (int i = 0; i < totalSamples; i++)
  {
    fprintf (dataout, "%f%c", data[i], " \n"[i != totalSamples - 1]);
  }
  fclose (dataout);
  /* Demean */
  float mean, std;
  getMeanAndSD (data, totalSamples, &mean, &std);
  for (int i = 0; i < totalSamples; i++)
  {
    data[i] -= mean;
  }
  /* Detrend */
  data_t *detrended;
  detrend (data, (int)totalSamples, &detrended);
  FILE *detrendout = fopen ("detrendout.txt", "w");
  for (int i = 0; i < totalSamples; i++)
  {
    fprintf (detrendout, "%f%c", detrended[i], " \n"[i != totalSamples - 1]);
  }
  fclose (detrendout);
  /* First taper the signal with 5%-cosine-window */
  float *taperedSignal = (float *)malloc (sizeof (float) * totalSamples);
  cosineTaper (detrended, (int)totalSamples, 0.05, taperedSignal);
  double *tapered   = (double *)malloc (sizeof (double) * totalSamples);
  FILE *taperResult = fopen ("taperout.txt", "w");
  for (int i = 0; i < totalSamples; i++)
  {
    tapered[i] = (double)taperedSignal[i];
    fprintf (taperResult, "%lf\n", tapered[i]);
  }
  fclose (taperResult);
  /* Then execute FFT */
  double complex *fftResult;
  fft (tapered, totalSamples, &fftResult);

  double delta         = 1. / sampleRate;
  double totalDuration = delta * totalSamples;

  FILE *fft_out = fopen ("fft_result.txt", "w");
  for (int i = 0; i < totalSamples; i++)
  {
    double frequency = i / totalDuration;
    fprintf (fft_out, "%lf %lf %lf\n", frequency, creal (fftResult[i]), cimag (fftResult[i]));
  }
  fclose (fft_out);

  /* instrument response removal */
  /* Read SACPZ file */
  //const char *sacpzfile = "./tests/SAC_PZs_TW_NACB_BHZ__2007.254.07.25.20.0000_99999.9999.24.60.60.99999";
  double complex *poles, *zeros;
  int npoles, nzeros;
  double constant;
  parse_sacpz (sacpzfile,
               &poles, &npoles,
               &zeros, &nzeros,
               &constant);
  /* Get frequency response */
  double *freq;
  double complex *freqResponse;
  get_freq_response (poles, npoles,
                     zeros, nzeros,
                     constant, sampleRate, totalSamples,
                     &freq, &freqResponse, totype);

  /* Apply freqyency response removal */
  remove_response (fftResult, freqResponse, totalSamples);
  FILE *freq_response_result = fopen ("freq_response_result.txt", "w");
  for (int i = 0; i < totalSamples; i++)
  {
    fprintf (freq_response_result, "%lf %e\n", freq[i], cabs (freqResponse[i]));
  }
  fclose (freq_response_result);
  FILE *response_removed = fopen ("response_removed.txt", "w");
  for (int i = 0; i < totalSamples; i++)
  {
    fprintf (response_removed, "%lf %lf\n", creal (fftResult[i]), cimag (fftResult[i]));
  }
  fclose (response_removed);

  /* band-pass filtering to prevent overamplification */
  double *taper_window;
  sacCosineTaper (freq, totalSamples, f1, f2, f3, f4, sampleRate, &taper_window);
  for (int i = 0; i < totalSamples; i++)
  {
    fftResult[i] *= taper_window[i];
  }

  /* Apply iFFT */
  double *output;
  ifft (fftResult, totalSamples, &output);
  /* Output signal with instrument response removed */
  FILE *out = fopen (outputFile, "w");
  if (out == NULL)
  {
    fprintf (stderr, "Cannot output signal output file.\n");
    return -1;
  }
  for (int i = 0; i < totalSamples; i++)
  {
    fprintf (out, "%e\n", output[i]);
  }
  fclose (out);

  /* Get power spetral density (PSD) of this segment */
  double *psd;

  /* Though McMarana 2004 mentions you should divide delta-t for each frequency response,
   * we do not do such operation as this produces resaonable result
   * Yet I still reverse this block for future use
   */
  /*for(int i = 0; i < totalSamples; i++) {
      fftResult[i] /= (1. / sampleRate);
  }*/

  rv = calculatePSD (fftResult, totalSamples, sampleRate, &psd);
  if (rv != 0)
  {
    fprintf (stderr, "Something wrong in calculate PSD procedure,\n");
    return -1;
  }
  FILE *psd_out = fopen ("psd_out.txt", "w");
  for (int i = 0; i < totalSamples; i++)
  {
    fprintf (psd_out, "%e %e\n", freq[i], psd[i]);
  }
  fclose (psd_out);

  /* Free allocated objects */
  free (data);
  free (detrended);
  free (taperedSignal);
  free (tapered);
  free (fftResult);
  free (poles);
  free (zeros);
  free (freq);
  free (freqResponse);
  free (psd);
  free (output);

  return 0;
}
