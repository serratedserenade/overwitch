/*
 *   main-cli.c
 *   Copyright (C) 2021 David García Goñi <dagargo@gmail.com>
 *
 *   This file is part of Overwitch.
 *
 *   Overwitch is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Overwitch is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Overwitch. If not, see <http://www.gnu.org/licenses/>.
 */

#include <signal.h>
#include <errno.h>
#include "../config.h"
#include "jclient.h"
#include "utils.h"
#include "common.h"

#define DEFAULT_QUALITY 2
#define DEFAULT_BLOCKS 24
#define DEFAULT_PRIORITY -1	//With this value the default priority will be used.

struct overwitch_instance
{
  pthread_t thread;
  struct jclient jclient;
};

static size_t instance_count;
static struct overwitch_instance *instances;

static struct option options[] = {
  {"use-device-number", 1, NULL, 'n'},
  {"use-device", 1, NULL, 'd'},
  {"resampling-quality", 1, NULL, 'q'},
  {"transfer-blocks", 1, NULL, 'b'},
  {"rt-priority", 1, NULL, 'p'},
  {"list-devices", 0, NULL, 'l'},
  {"verbose", 0, NULL, 'v'},
  {"help", 0, NULL, 'h'},
  {NULL, 0, NULL, 0}
};

static void
signal_handler (int signo)
{
  if (signo == SIGHUP || signo == SIGINT || signo == SIGTERM)
    {
      struct overwitch_instance *instance = instances;
      for (int i = 0; i < instance_count; i++, instance++)
	{
	  jclient_exit (&instance->jclient);
	}
    }
  else if (signo == SIGUSR1)
    {
      struct overwitch_instance *instance = instances;
      for (int i = 0; i < instance_count; i++, instance++)
	{
	  ow_resampler_report_status (instance->jclient.resampler);
	}
    }
}

static int
run_single (int device_num, const char *device_name,
	    int blocks_per_transfer, int quality, int priority)
{
  struct ow_usb_device *device;
  ow_err_t err = OW_OK;

  if (ow_get_usb_device_from_device_attrs (device_num, device_name, &device))
    {
      return OW_GENERIC_ERROR;
    }

  instance_count = 1;
  instances = malloc (sizeof (struct overwitch_instance));
  instances->jclient.bus = device->bus;
  instances->jclient.address = device->address;
  instances->jclient.blocks_per_transfer = blocks_per_transfer;
  instances->jclient.quality = quality;
  instances->jclient.priority = priority;
  instances->jclient.reporter.callback = NULL;
  instances->jclient.reporter.period = 2;
  instances->jclient.end_notifier = NULL;
  free (device);

  if (jclient_init (&instances->jclient))
    {
      err = OW_GENERIC_ERROR;
      goto end;
    }

  pthread_create (&instances->thread, NULL, jclient_run_thread,
		  &instances->jclient);
  pthread_join (instances->thread, NULL);

end:
  free (instances);
  return err;
}

static int
run_all (int blocks_per_transfer, int quality, int priority)
{
  struct ow_usb_device *devices;
  struct ow_usb_device *device;
  struct overwitch_instance *instance;
  ow_err_t err = ow_get_devices (&devices, &instance_count);

  if (err)
    {
      return err;
    }

  instances = malloc (sizeof (struct overwitch_instance) * instance_count);

  device = devices;
  instance = instances;
  for (int i = 0; i < instance_count; i++, instance++, device++)
    {
      instance->jclient.bus = device->bus;
      instance->jclient.address = device->address;
      instance->jclient.blocks_per_transfer = blocks_per_transfer;
      instance->jclient.quality = quality;
      instance->jclient.priority = priority;
      instance->jclient.reporter.callback = NULL;
      instance->jclient.reporter.period = 2;
      instance->jclient.end_notifier = NULL;

      if (jclient_init (&instance->jclient))
	{
	  continue;
	}

      pthread_create (&instance->thread, NULL, jclient_run_thread,
		      &instance->jclient);
    }

  ow_free_usb_device_list (devices, instance_count);

  instance = instances;
  for (int i = 0; i < instance_count; i++, instance++)
    {
      pthread_join (instance->thread, NULL);
    }

  free (instances);

  return OW_OK;
}

int
main (int argc, char *argv[])
{
  int opt;
  int vflg = 0, lflg = 0, dflg = 0, bflg = 0, pflg = 0, nflg = 0, errflg = 0;
  char *endstr;
  char *device_name = NULL;
  int long_index = 0;
  ow_err_t ow_err;
  struct sigaction action;
  int device_num = -1;
  int blocks_per_transfer = DEFAULT_BLOCKS;
  int quality = DEFAULT_QUALITY;
  int priority = DEFAULT_PRIORITY;

  action.sa_handler = signal_handler;
  sigemptyset (&action.sa_mask);
  action.sa_flags = 0;
  sigaction (SIGHUP, &action, NULL);
  sigaction (SIGINT, &action, NULL);
  sigaction (SIGTERM, &action, NULL);
  sigaction (SIGUSR1, &action, NULL);

  while ((opt = getopt_long (argc, argv, "n:d:q:b:p:lvh",
			     options, &long_index)) != -1)
    {
      switch (opt)
	{
	case 'n':
	  device_num = (int) strtol (optarg, &endstr, 10);
	  nflg++;
	  break;
	case 'd':
	  device_name = optarg;
	  dflg++;
	  break;
	case 'q':
	  quality = (int) strtol (optarg, &endstr, 10);
	  if (errno || endstr == optarg || *endstr != '\0' || quality > 4
	      || quality < 0)
	    {
	      quality = DEFAULT_QUALITY;
	      fprintf (stderr,
		       "Resampling quality value must be in [0..4]. Using value %d...\n",
		       quality);
	    }
	  break;
	case 'b':
	  blocks_per_transfer = (int) strtol (optarg, &endstr, 10);
	  if (errno || endstr == optarg || *endstr != '\0'
	      || blocks_per_transfer < 2 || blocks_per_transfer > 32)
	    {
	      blocks_per_transfer = DEFAULT_BLOCKS;
	      fprintf (stderr,
		       "Blocks value must be in [2..32]. Using value %d...\n",
		       blocks_per_transfer);
	    }
	  bflg++;
	  break;
	case 'p':
	  priority = (int) strtol (optarg, &endstr, 10);
	  if (errno || endstr == optarg || *endstr != '\0' || priority < 0
	      || priority > 99)
	    {
	      priority = -1;
	      fprintf (stderr,
		       "Priority value must be in [0..99]. Using default JACK value...\n");
	    }
	  pflg++;
	  break;
	case 'l':
	  lflg++;
	  break;
	case 'v':
	  vflg++;
	  break;
	case 'h':
	  print_help (argv[0], PACKAGE_STRING, options);
	  exit (EXIT_SUCCESS);
	case '?':
	  errflg++;
	}
    }

  if (errflg > 0)
    {
      print_help (argv[0], PACKAGE_STRING, options);
      exit (EXIT_FAILURE);
    }

  if (vflg)
    {
      debug_level = vflg;
    }

  if (lflg)
    {
      ow_err = print_devices ();
      if (ow_err)
	{
	  fprintf (stderr, "USB error: %s\n", ow_get_err_str (ow_err));
	  exit (EXIT_FAILURE);
	}
      exit (EXIT_SUCCESS);
    }

  if (bflg > 1)
    {
      fprintf (stderr, "Undetermined blocks\n");
      exit (EXIT_FAILURE);
    }

  if (pflg > 1)
    {
      fprintf (stderr, "Undetermined priority\n");
      exit (EXIT_FAILURE);
    }

  if (nflg + dflg == 0)
    {
      return run_all (blocks_per_transfer, quality, priority);
    }
  else if (nflg + dflg == 1)
    {
      return run_single (device_num, device_name,
			 blocks_per_transfer, quality, priority);
    }
  else
    {
      fprintf (stderr, "Device not provided properly\n");
      exit (EXIT_FAILURE);
    }

  return EXIT_SUCCESS;
}
