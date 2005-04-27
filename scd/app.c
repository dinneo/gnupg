/* app.c - Application selection.
 *	Copyright (C) 2003, 2004, 2005 Free Software Foundation, Inc.
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA
 */

#include <config.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "scdaemon.h"
#include "app-common.h"
#include "apdu.h"
#include "iso7816.h"
#include "tlv.h"


/* Check wether the application NAME is allowed.  This does not mean
   we have support for it though.  */
static int
is_app_allowed (const char *name)
{
  strlist_t l;

  for (l=opt.disabled_applications; l; l = l->next)
    if (!strcmp (l->d, name))
      return 0; /* no */
  return 1; /* yes */
}


/* If called with NAME as NULL, select the best fitting application
   and return a context; otherwise select the application with NAME
   and return a context.  SLOT identifies the reader device. Returns
   an error code and stores NULL at R_APP if no application was found
   or no card is present. */
gpg_error_t
select_application (ctrl_t ctrl, int slot, const char *name, app_t *r_app)
{
  int rc;
  app_t app;
  unsigned char *result = NULL;
  size_t resultlen;

  *r_app = NULL;
  app = xtrycalloc (1, sizeof *app);
  if (!app)
    {
      rc = gpg_error_from_errno (errno);
      log_info ("error allocating context: %s\n", gpg_strerror (rc));
      return rc;
    }
  app->slot = slot;

  /* Fixme: We should now first check whether a card is at all
     present. */

  /* Try to read the GDO file first to get a default serial number. */
  rc = iso7816_select_file (slot, 0x3F00, 1, NULL, NULL);
  if (!rc)
    rc = iso7816_select_file (slot, 0x2F02, 0, NULL, NULL);
  if (!rc)
     rc = iso7816_read_binary (slot, 0, 0, &result, &resultlen);
  if (!rc)
    {
      size_t n;
      const unsigned char *p;

      p = find_tlv_unchecked (result, resultlen, 0x5A, &n);
      if (p)
        resultlen -= (p-result);
      if (p && n > resultlen && n == 0x0d && resultlen+1 == n)
        {
          /* The object it does not fit into the buffer.  This is an
             invalid encoding (or the buffer is too short.  However, I
             have some test cards with such an invalid encoding and
             therefore I use this ugly workaround to return something
             I can further experiment with. */
          log_debug ("enabling BMI testcard workaround\n");
          n--;
        }

      if (p && n <= resultlen)
        {
          /* The GDO file is pretty short, thus we simply reuse it for
             storing the serial number. */
          memmove (result, p, n);
          app->serialno = result;
          app->serialnolen = n;
          rc = app_munge_serialno (app);
          if (rc)
            goto leave;
        }
      else
        xfree (result);
      result = NULL;
    }

  /* For certain error codes, there is no need to try more.  */
  if (gpg_err_code (rc) == GPG_ERR_CARD_NOT_PRESENT)
    goto leave;
  

  /* Figure out the application to use.  */
  rc = gpg_error (GPG_ERR_NOT_FOUND);

  if (rc && is_app_allowed ("openpgp") && (!name || !strcmp (name, "openpgp")))
    rc = app_select_openpgp (app);
  if (rc && is_app_allowed ("nks") && (!name || !strcmp (name, "nks")))
    rc = app_select_nks (app);
  if (rc && is_app_allowed ("p15") && (!name || !strcmp (name, "p15")))
    rc = app_select_p15 (app);
  if (rc && is_app_allowed ("dinsig") && (!name || !strcmp (name, "dinsig")))
    rc = app_select_dinsig (app);
  if (rc && name)
    rc = gpg_error (GPG_ERR_NOT_SUPPORTED);

 leave:
  if (rc)
    {
      if (name)
        log_info ("can't select application `%s': %s\n",
                  name, gpg_strerror (rc));
      else
        log_info ("no supported card application found: %s\n",
                  gpg_strerror (rc));
      xfree (app);
      return rc;
    }

  app->initialized = 1;
  *r_app = app;
  return 0;
}


void
release_application (app_t app)
{
  if (!app)
    return;

  if (app->fnc.deinit)
    {
      app->fnc.deinit (app);
      app->fnc.deinit = NULL;
    }

  xfree (app->serialno);
  xfree (app);
}



/* The serial number may need some cosmetics.  Do it here.  This
   function shall only be called once after a new serial number has
   been put into APP->serialno. 

   Prefixes we use:
   
     FF 00 00 = For serial numbers starting with an FF
     FF 01 00 = Some german p15 cards return an empty serial number so the
                serial number from the EF(TokenInfo) is used instead.
     
     All other serial number not starting with FF are used as they are.
*/
int
app_munge_serialno (app_t app)
{
  if (app->serialnolen && app->serialno[0] == 0xff)
    { 
      /* The serial number starts with our special prefix.  This
         requires that we put our default prefix "FF0000" in front. */
      unsigned char *p = xtrymalloc (app->serialnolen + 3);
      if (!p)
        return gpg_error (gpg_err_code_from_errno (errno));
      memcpy (p, "\xff\0", 3);
      memcpy (p+3, app->serialno, app->serialnolen);
      app->serialnolen += 3;
      xfree (app->serialno);
      app->serialno = p;
    }
  return 0;
}



/* Retrieve the serial number and the time of the last update of the
   card.  The serial number is returned as a malloced string (hex
   encoded) in SERIAL and the time of update is returned in STAMP.  If
   no update time is available the returned value is 0.  Caller must
   free SERIAL unless the function returns an error.  If STAMP is not
   of interest, NULL may be passed. */
int 
app_get_serial_and_stamp (app_t app, char **serial, time_t *stamp)
{
  unsigned char *buf, *p;
  int i;

  if (!app || !serial)
    return gpg_error (GPG_ERR_INV_VALUE);

  *serial = NULL;
  if (stamp)
    *stamp = 0; /* not available */

  buf = xtrymalloc (app->serialnolen * 2 + 1);
  if (!buf)
    return gpg_error_from_errno (errno);
  for (p=buf, i=0; i < app->serialnolen; p +=2, i++)
    sprintf (p, "%02X", app->serialno[i]);
  *p = 0;
  *serial = buf;
  return 0;
}


/* Write out the application specifig status lines for the LEARN
   command. */
int
app_write_learn_status (APP app, CTRL ctrl)
{
  if (!app)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.learn_status)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);

  if (app->apptype)
    send_status_info (ctrl, "APPTYPE",
                      app->apptype, strlen (app->apptype), NULL, 0);

  return app->fnc.learn_status (app, ctrl);
}


/* Read the certificate with id CERTID (as returned by learn_status in
   the CERTINFO status lines) and return it in the freshly allocated
   buffer put into CERT and the length of the certificate put into
   CERTLEN. */
int
app_readcert (app_t app, const char *certid,
              unsigned char **cert, size_t *certlen)
{
  if (!app)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.readcert)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);

  return app->fnc.readcert (app, certid, cert, certlen);
}


/* Read the key with ID KEYID.  On success a canonical encoded
   S-expression with the public key will get stored at PK and its
   length (for assertions) at PKLEN; the caller must release that
   buffer. On error NULL will be stored at PK and PKLEN and an error
   code returned.

   This function might not be supported by all applications.  */
int
app_readkey (app_t app, const char *keyid, unsigned char **pk, size_t *pklen)
{
  if (pk)
    *pk = NULL;
  if (pklen)
    *pklen = 0;

  if (!app || !keyid || !pk || !pklen)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.readkey)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);

  return app->fnc.readkey (app, keyid, pk, pklen);
}


/* Perform a GETATTR operation.  */
int 
app_getattr (APP app, CTRL ctrl, const char *name)
{
  if (!app || !name || !*name)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);

  if (app->apptype && name && !strcmp (name, "APPTYPE"))
    {
      send_status_info (ctrl, "APPTYPE",
                        app->apptype, strlen (app->apptype), NULL, 0);
      return 0;
    }
  if (name && !strcmp (name, "SERIALNO"))
    {
      char *serial;
      time_t stamp;
      int rc;
      
      rc = app_get_serial_and_stamp (app, &serial, &stamp);
      if (rc)
        return rc;
      send_status_info (ctrl, "SERIALNO", serial, strlen (serial), NULL, 0);
      xfree (serial);
      return 0;
    }

  if (!app->fnc.getattr)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);
  return app->fnc.getattr (app, ctrl, name);
}

/* Perform a SETATTR operation.  */
int 
app_setattr (APP app, const char *name,
             int (*pincb)(void*, const char *, char **),
             void *pincb_arg,
             const unsigned char *value, size_t valuelen)
{
  if (!app || !name || !*name || !value)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.setattr)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);
  return app->fnc.setattr (app, name, pincb, pincb_arg, value, valuelen);
}

/* Create the signature and return the allocated result in OUTDATA.
   If a PIN is required the PINCB will be used to ask for the PIN; it
   should return the PIN in an allocated buffer and put it into PIN.  */
int 
app_sign (APP app, const char *keyidstr, int hashalgo,
          int (pincb)(void*, const char *, char **),
          void *pincb_arg,
          const void *indata, size_t indatalen,
          unsigned char **outdata, size_t *outdatalen )
{
  int rc;

  if (!app || !indata || !indatalen || !outdata || !outdatalen || !pincb)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.sign)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);
  rc = app->fnc.sign (app, keyidstr, hashalgo,
                      pincb, pincb_arg,
                      indata, indatalen,
                      outdata, outdatalen);
  if (opt.verbose)
    log_info ("operation sign result: %s\n", gpg_strerror (rc));
  return rc;
}

/* Create the signature using the INTERNAL AUTHENTICATE command and
   return the allocated result in OUTDATA.  If a PIN is required the
   PINCB will be used to ask for the PIN; it should return the PIN in
   an allocated buffer and put it into PIN.  */
int 
app_auth (APP app, const char *keyidstr,
          int (pincb)(void*, const char *, char **),
          void *pincb_arg,
          const void *indata, size_t indatalen,
          unsigned char **outdata, size_t *outdatalen )
{
  int rc;

  if (!app || !indata || !indatalen || !outdata || !outdatalen || !pincb)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.auth)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);
  rc = app->fnc.auth (app, keyidstr,
                      pincb, pincb_arg,
                      indata, indatalen,
                      outdata, outdatalen);
  if (opt.verbose)
    log_info ("operation auth result: %s\n", gpg_strerror (rc));
  return rc;
}


/* Decrypt the data in INDATA and return the allocated result in OUTDATA.
   If a PIN is required the PINCB will be used to ask for the PIN; it
   should return the PIN in an allocated buffer and put it into PIN.  */
int 
app_decipher (APP app, const char *keyidstr,
              int (pincb)(void*, const char *, char **),
              void *pincb_arg,
              const void *indata, size_t indatalen,
              unsigned char **outdata, size_t *outdatalen )
{
  int rc;

  if (!app || !indata || !indatalen || !outdata || !outdatalen || !pincb)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.decipher)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);
  rc = app->fnc.decipher (app, keyidstr,
                          pincb, pincb_arg,
                          indata, indatalen,
                          outdata, outdatalen);
  if (opt.verbose)
    log_info ("operation decipher result: %s\n", gpg_strerror (rc));
  return rc;
}


/* Perform a SETATTR operation.  */
int 
app_genkey (APP app, CTRL ctrl, const char *keynostr, unsigned int flags,
            int (*pincb)(void*, const char *, char **),
            void *pincb_arg)
{
  int rc;

  if (!app || !keynostr || !*keynostr || !pincb)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.genkey)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);
  rc = app->fnc.genkey (app, ctrl, keynostr, flags, pincb, pincb_arg);
  if (opt.verbose)
    log_info ("operation genkey result: %s\n", gpg_strerror (rc));
  return rc;
}


/* Perform a GET CHALLENGE operation.  This fucntion is special as it
   directly accesses the card without any application specific
   wrapper. */
int
app_get_challenge (APP app, size_t nbytes, unsigned char *buffer)
{
  if (!app || !nbytes || !buffer)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  return iso7816_get_challenge (app->slot, nbytes, buffer);
}



/* Perform a CHANGE REFERENCE DATA or RESET RETRY COUNTER operation.  */
int 
app_change_pin (APP app, CTRL ctrl, const char *chvnostr, int reset_mode,
                int (*pincb)(void*, const char *, char **),
                void *pincb_arg)
{
  int rc;

  if (!app || !chvnostr || !*chvnostr || !pincb)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.change_pin)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);
  rc = app->fnc.change_pin (app, ctrl, chvnostr, reset_mode, pincb, pincb_arg);
  if (opt.verbose)
    log_info ("operation change_pin result: %s\n", gpg_strerror (rc));
  return rc;
}


/* Perform a VERIFY operation without doing anything lese.  This may
   be used to initialze a the PIN cache for long lasting other
   operations.  Its use is highly application dependent. */
int 
app_check_pin (APP app, const char *keyidstr,
               int (*pincb)(void*, const char *, char **),
               void *pincb_arg)
{
  int rc;

  if (!app || !keyidstr || !*keyidstr || !pincb)
    return gpg_error (GPG_ERR_INV_VALUE);
  if (!app->initialized)
    return gpg_error (GPG_ERR_CARD_NOT_INITIALIZED);
  if (!app->fnc.check_pin)
    return gpg_error (GPG_ERR_UNSUPPORTED_OPERATION);
  rc = app->fnc.check_pin (app, keyidstr, pincb, pincb_arg);
  if (opt.verbose)
    log_info ("operation check_pin result: %s\n", gpg_strerror (rc));
  return rc;
}

