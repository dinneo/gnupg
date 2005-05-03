/* passphrase.c -  Get a passphrase
 * Copyright (C) 1998, 1999, 2000, 2001, 2002, 2003,
 *               2004, 2005 Free Software Foundation, Inc.
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
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#if !defined(HAVE_DOSISH_SYSTEM) && !defined(__riscos__)
#include <sys/socket.h>
#include <sys/un.h>
#endif
#if defined (_WIN32)
#include <windows.h>
#endif
#include <errno.h>
#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif
#ifdef HAVE_LANGINFO_CODESET
#include <langinfo.h>
#endif

#include "util.h"
#include "memory.h"
#include "options.h"
#include "ttyio.h"
#include "cipher.h"
#include "keydb.h"
#include "main.h"
#include "i18n.h"
#include "status.h"
#ifdef ENABLE_AGENT_SUPPORT
#include "assuan.h"
#endif /*ENABLE_AGENT_SUPPORT*/


#define buftou32( p )  ((*(byte*)(p) << 24) | (*((byte*)(p)+1)<< 16) | \
		       (*((byte*)(p)+2) << 8) | (*((byte*)(p)+3)))
#define u32tobuf( p, a ) do { 			                \
			    ((byte*)p)[0] = (byte)((a) >> 24);	\
			    ((byte*)p)[1] = (byte)((a) >> 16);	\
			    ((byte*)p)[2] = (byte)((a) >>  8);	\
			    ((byte*)p)[3] = (byte)((a) 	    );	\
			} while(0)

#define digitp(p)   (*(p) >= '0' && *(p) <= '9')
#define hexdigitp(a) (digitp (a)                     \
                      || (*(a) >= 'A' && *(a) <= 'F')  \
                      || (*(a) >= 'a' && *(a) <= 'f'))
#define xtoi_1(p)   (*(p) <= '9'? (*(p)- '0'): \
                     *(p) <= 'F'? (*(p)-'A'+10):(*(p)-'a'+10))
#define xtoi_2(p)   ((xtoi_1(p) * 16) + xtoi_1((p)+1))



static char *fd_passwd = NULL;
static char *next_pw = NULL;
static char *last_pw = NULL;

#if defined (_WIN32)
static int read_fd = 0;
static int write_fd = 0;
#endif

static void hash_passphrase( DEK *dek, char *pw, STRING2KEY *s2k, int create );

int
have_static_passphrase()
{
    if ( opt.use_agent )
        return 0;
    return !!fd_passwd;
}

/****************
 * Set the passphrase to be used for the next query and only for the next
 * one.
 */
void
set_next_passphrase( const char *s )
{
    m_free(next_pw);
    next_pw = NULL;
    if( s ) {
	next_pw = m_alloc_secure( strlen(s)+1 );
	strcpy(next_pw, s );
    }
}

/****************
 * Get the last passphrase used in passphrase_to_dek.
 * Note: This removes the passphrase from this modules and
 * the caller must free the result.  May return NULL:
 */
char *
get_last_passphrase()
{
    char *p = last_pw;
    last_pw = NULL;
    return p;
}


void
read_passphrase_from_fd( int fd )
{
  int i, len;
  char *pw;
  
  if ( opt.use_agent ) 
    { /* Not used but we have to do a dummy read, so that it won't end
         up at the begin of the message if the quite usual trick to
         prepend the passphtrase to the message is used. */
      char buf[1];

      while (!(read (fd, buf, 1) != 1 || *buf == '\n' ))
        ;
      *buf = 0;
      return; 
    }

  if (!opt.batch )
	tty_printf("Reading passphrase from file descriptor %d ...", fd );
  for (pw = NULL, i = len = 100; ; i++ ) 
    {
      if (i >= len-1 ) 
        {
          char *pw2 = pw;
          len += 100;
          pw = m_alloc_secure( len );
          if( pw2 )
            {
              memcpy(pw, pw2, i );
              m_free (pw2);
            }
          else
            i=0;
	}
      if (read( fd, pw+i, 1) != 1 || pw[i] == '\n' )
        break;
    }
  pw[i] = 0;
  if (!opt.batch)
    tty_printf("\b\b\b   \n" );

  m_free( fd_passwd );
  fd_passwd = pw;
}



#ifdef ENABLE_AGENT_SUPPORT
/* Send one option to the gpg-agent.  */
static int
agent_send_option (assuan_context_t ctx, const char *name, const char *value)
{
  char *line;
  int rc; 
  
  if (!value || !*value)
    return 0; /* Avoid sending empty option values. */

  line = xmalloc (7 + strlen (name) + 1 + strlen (value) + 1);
  strcpy (stpcpy (stpcpy (stpcpy (line, "OPTION "), name), "="), value);
  rc = assuan_transact (ctx, line, NULL, NULL, NULL, NULL, NULL, NULL);
  xfree (line);
  return rc? -1 : 0;
}

/* Send all required options to the gpg-agent.  */
static int 
agent_send_all_options (assuan_context_t ctx)
{
  char *dft_display = NULL;
  const char *dft_ttyname = NULL;
  char *dft_ttytype = NULL;
  char *old_lc = NULL;
  char *dft_lc = NULL;
  int rc = 0;

  dft_display = getenv ("DISPLAY");
  if (opt.display || dft_display)
    {
      if (agent_send_option (ctx, "display",
                             opt.display ? opt.display : dft_display))
        return -1;
    }

  if (!opt.ttyname)
    {
      const char *tmp;

      dft_ttyname = getenv ("GPG_TTY");
      if ((!dft_ttyname || !*dft_ttyname) && (tmp=ttyname (0)))
        dft_ttyname = tmp;
      if ((!dft_ttyname || !*dft_ttyname) && (tmp=tty_get_ttyname ()))
        dft_ttyname = tmp;
    }
  if (opt.ttyname || dft_ttyname)
    {
      if (agent_send_option (ctx, "ttyname",
                             opt.ttyname ? opt.ttyname : dft_ttyname))
        return -1;
    }

  dft_ttytype = getenv ("TERM");
  if (opt.ttytype || (dft_ttyname && dft_ttytype))
    {
      if (agent_send_option (ctx, "ttytype",
                             opt.ttyname ? opt.ttytype : dft_ttytype))
        return -1;
    }

#if defined(HAVE_SETLOCALE) && defined(LC_CTYPE)
  old_lc = setlocale (LC_CTYPE, NULL);
  if (old_lc)
    old_lc = m_strdup (old_lc);
  dft_lc = setlocale (LC_CTYPE, "");
#endif
  if (opt.lc_ctype || (dft_ttyname && dft_lc))
    {
      rc = agent_send_option (ctx, "lc-ctype",
                              opt.lc_ctype ? opt.lc_ctype : dft_lc);
    }
#if defined(HAVE_SETLOCALE) && defined(LC_CTYPE)
  if (old_lc)
    {
      setlocale (LC_CTYPE, old_lc);
      m_free (old_lc);
    }
#endif
  if (rc)
    return rc;

#if defined(HAVE_SETLOCALE) && defined(LC_MESSAGES)
  old_lc = setlocale (LC_MESSAGES, NULL);
  if (old_lc)
    old_lc = m_strdup (old_lc);
  dft_lc = setlocale (LC_MESSAGES, "");
#endif
  if (opt.lc_messages || (dft_ttyname && dft_lc))
    {
      rc = agent_send_option (ctx, "lc-messages",
                              opt.lc_messages ? opt.lc_messages : dft_lc);
    }
#if defined(HAVE_SETLOCALE) && defined(LC_MESSAGES)
  if (old_lc)
    {
      setlocale (LC_MESSAGES, old_lc);
      m_free (old_lc);
    }
#endif
  return rc;
}
#endif /*ENABLE_AGENT_SUPPORT*/


/*
 * Open a connection to the agent and initializes the connection.
 * Returns: -1 on error; on success a file descriptor for that
 * connection is returned.
 */
#ifdef ENABLE_AGENT_SUPPORT
static assuan_context_t
agent_open (void)
{
  int rc;
  assuan_context_t ctx;
  char *infostr, *p;
  int prot;
  int pid;

  if (opt.gpg_agent_info)
    infostr = xstrdup (opt.gpg_agent_info);
  else
    {
      infostr = getenv ( "GPG_AGENT_INFO" );
      if (!infostr || !*infostr) 
        {
          log_error (_("gpg-agent is not available in this session\n"));
          opt.use_agent = 0;
          return NULL;
        }
      infostr = xstrdup ( infostr );
    }
  
  if ( !(p = strchr (infostr, PATHSEP_C)) || p == infostr)
    {
      log_error ( _("malformed GPG_AGENT_INFO environment variable\n"));
      xfree (infostr);
      opt.use_agent = 0;
      return NULL;
    }
  *p++ = 0;
  pid = atoi (p);
  while (*p && *p != PATHSEP_C)
    p++;
  prot = *p? atoi (p+1) : 0;
  if (prot != 1)
    {
      log_error (_("gpg-agent protocol version %d is not supported\n"), prot);
      xfree (infostr);
      opt.use_agent = 0;
      return NULL;
    }
     
  rc = assuan_socket_connect (&ctx, infostr, pid);
  if (rc)
    {
      log_error ( _("can't connect to `%s': %s\n"), 
                  infostr, assuan_strerror (rc));
      xfree (infostr );
      opt.use_agent = 0;
      return NULL;
    }
  xfree (infostr);

  if (agent_send_all_options (ctx))
    {
      log_error (_("problem with the agent - disabling agent use\n"));
      assuan_disconnect (ctx);
      opt.use_agent = 0;
      return NULL;
    }

  return ctx;
}
#endif/*ENABLE_AGENT_SUPPORT*/


#ifdef ENABLE_AGENT_SUPPORT
static void
agent_close (assuan_context_t ctx)
{
  assuan_disconnect (ctx);
}
#endif /*ENABLE_AGENT_SUPPORT*/


/* Copy the text ATEXT into the buffer P and do plus '+' and percent
   escaping.  Note that the provided buffer needs to be 3 times the
   size of ATEXT plus 1.  Returns a pointer to the leading Nul in P. */
#ifdef ENABLE_AGENT_SUPPORT
static char *
percent_plus_escape (char *p, const char *atext)
{
  const unsigned char *s;

  for (s=atext; *s; s++)
    {
      if (*s < ' ' || *s == '+')
        {
          sprintf (p, "%%%02X", *s);
          p += 3;
        }
      else if (*s == ' ')
        *p++ = '+';
      else
        *p++ = *s;
    }
  *p = 0;
  return p;
}
#endif /*ENABLE_AGENT_SUPPORT*/


#ifdef ENABLE_AGENT_SUPPORT

/* Object for the agent_okay_cb function.  */
struct agent_okay_cb_s {
  char *pw;
};

/* A callback used to get the passphrase from the okay line.  See
   agent-get_passphrase for details.  LINE is the rest of the OK
   status line without leading white spaces. */
static assuan_error_t
agent_okay_cb (void *opaque, const char *line)
{ 
  struct agent_okay_cb_s *parm = opaque;
  int i;

  /* Note: If the malloc below fails we won't be able to wipe the
     memory at LINE given the current implementation of the Assuan
     code. There is no easy ay around this w/o adding a lot of more
     memory function code to allow wiping arbitrary stuff on memory
     failure. */
  parm->pw = xmalloc_secure (strlen (line)/2+2);
  
  for (i=0; hexdigitp (line) && hexdigitp (line+1); line += 2)
    parm->pw[i++] = xtoi_2 (line);
  parm->pw[i] = 0; 
  return 0;
}
#endif /*ENABLE_AGENT_SUPPORT*/



/*
 * Ask the GPG Agent for the passphrase.
 * Mode 0:  Allow cached passphrase
 *      1:  No cached passphrase FIXME: Not really implemented
 *      2:  Ditto, but change the text to "repeat entry"
 *
 * Note that TRYAGAIN_TEXT must not be translated.  If canceled is not
 * NULL, the function does set it to 1 if the user canceled the
 * operation.  If CACHEID is not NULL, it will be used as the cacheID
 * for the gpg-agent; if is NULL and a key fingerprint can be
 * computed, this will be used as the cacheid.
 */
static char *
agent_get_passphrase ( u32 *keyid, int mode, const char *cacheid,
                       const char *tryagain_text,
                       const char *custom_description,
                       const char *custom_prompt, int *canceled)
{
#ifdef ENABLE_AGENT_SUPPORT
  char *atext = NULL;
  assuan_context_t ctx = NULL;
  char *pw = NULL;
  PKT_public_key *pk = m_alloc_clear( sizeof *pk );
  byte fpr[MAX_FINGERPRINT_LEN];
  int have_fpr = 0;
  char *orig_codeset = NULL;

  if (canceled)
    *canceled = 0;

#if MAX_FINGERPRINT_LEN < 20
#error agent needs a 20 byte fingerprint
#endif

  memset (fpr, 0, MAX_FINGERPRINT_LEN );
  if( keyid && get_pubkey( pk, keyid ) )
    {
      if (pk)
        free_public_key( pk );      
      pk = NULL; /* oops: no key for some reason */
    }
  
#ifdef ENABLE_NLS
  /* The Assuan agent protocol requires us to transmit utf-8 strings */
  orig_codeset = bind_textdomain_codeset (PACKAGE, NULL);
#ifdef HAVE_LANGINFO_CODESET
  if (!orig_codeset)
    orig_codeset = nl_langinfo (CODESET);
#endif
  if (orig_codeset)
    { /* We only switch when we are able to restore the codeset later. */
      orig_codeset = m_strdup (orig_codeset);
      if (!bind_textdomain_codeset (PACKAGE, "utf-8"))
        orig_codeset = NULL; 
    }
#endif

  if ( !(ctx = agent_open ()) ) 
    goto failure;

  if (custom_description)
    atext = native_to_utf8 (custom_description);
  else if ( !mode && pk && keyid )
    { 
      char *uid;
      size_t uidlen;
      const char *algo_name = pubkey_algo_to_string ( pk->pubkey_algo );
      const char *timestr;
      char *maink;
      
      if ( !algo_name )
        algo_name = "?";

#define KEYIDSTRING _(" (main key ID %s)")

      maink = m_alloc ( strlen (KEYIDSTRING) + keystrlen() + 20 );
      if( keyid[2] && keyid[3] && keyid[0] != keyid[2] 
          && keyid[1] != keyid[3] )
        sprintf( maink, KEYIDSTRING, keystr(&keyid[2]) );
      else
        *maink = 0;
      
      uid = get_user_id ( keyid, &uidlen ); 
      timestr = strtimestamp (pk->timestamp);

#undef KEYIDSTRING

#define PROMPTSTRING _("You need a passphrase to unlock the secret" \
		       " key for user:\n" \
		       "\"%.*s\"\n" \
		       "%u-bit %s key, ID %s, created %s%s\n" )

      atext = m_alloc ( 100 + strlen (PROMPTSTRING)  
                        + uidlen + 15 + strlen(algo_name) + keystrlen()
                        + strlen (timestr) + strlen (maink) );
      sprintf (atext, PROMPTSTRING,
               (int)uidlen, uid,
               nbits_from_pk (pk), algo_name, keystr(&keyid[0]), timestr,
               maink  );
      m_free (uid);
      m_free (maink);

#undef PROMPTSTRING

      { 
        size_t dummy;
        fingerprint_from_pk( pk, fpr, &dummy );
        have_fpr = 1;
      }
      
    }
  else if (mode == 2 ) 
    atext = m_strdup ( _("Repeat passphrase\n") );
  else
    atext = m_strdup ( _("Enter passphrase\n") );
                
  { 
      char *line, *p;
      int i, rc; 
      struct agent_okay_cb_s okay_cb_parm;

      if (!tryagain_text)
        tryagain_text = "X";
      else
        tryagain_text = _(tryagain_text);

      /* We allocate 23 times the needed space for thye texts so that
         there is enough space for escaping. */
      line = xmalloc (15 + 46 
                      + 3*strlen (atext)
                      + 3*strlen (custom_prompt? custom_prompt:"")
                      + (cacheid? (3*strlen (cacheid)): 0)
                      + 3*strlen (tryagain_text)
                      + 1);
      strcpy (line, "GET_PASSPHRASE ");
      p = line+15;
      if (!mode && cacheid)
        {
          p = percent_plus_escape (p, cacheid);
        }
      else if (!mode && have_fpr)
        {
          for (i=0; i < 20; i++, p +=2 )
            sprintf (p, "%02X", fpr[i]);
        }
      else
        *p++ = 'X'; /* No caching. */
      *p++ = ' ';

      p = percent_plus_escape (p, tryagain_text);
      *p++ = ' ';

      /* The prompt.  */
      if (custom_prompt)
        {
          char *tmp = native_to_utf8 (custom_prompt);
          p = percent_plus_escape (p, tmp);
          xfree (tmp);
        }
      else
        *p++ = 'X'; /* Use the standard prompt. */
      *p++ = ' ';

      /* Copy description. */
      percent_plus_escape (p, atext);

      /* Call gpg-agent.  */
      memset (&okay_cb_parm, 0, sizeof okay_cb_parm);
      rc = assuan_transact2 (ctx, line, NULL, NULL, NULL, NULL, NULL, NULL,
                             agent_okay_cb, &okay_cb_parm);

      xfree (line);
      xfree (atext); atext = NULL;
      if (!rc)
        {
          assert (okay_cb_parm.pw);
          pw = okay_cb_parm.pw;
          agent_close (ctx);
          if (pk)
            free_public_key( pk );
#ifdef ENABLE_NLS
          if (orig_codeset)
            bind_textdomain_codeset (PACKAGE, orig_codeset);
#endif
          xfree (orig_codeset);
          return pw;
        }
      else if (rc && (rc & 0xffff) == 99)
	{
	  /* 99 is GPG_ERR_CANCELED. */
          log_info (_("cancelled by user\n") );
          if (canceled)
            *canceled = 1;
        }
      else 
        {
          log_error (_("problem with the agent - disabling agent use\n"));
          opt.use_agent = 0;
        }
  }
      
        
 failure:
#ifdef ENABLE_NLS
  if (orig_codeset)
    bind_textdomain_codeset (PACKAGE, orig_codeset);
#endif
  xfree (atext);
  agent_close (ctx);
  xfree (pw );
  if (pk)
    free_public_key( pk );

#endif /*ENABLE_AGENT_SUPPORT*/

  return NULL;
}


/*
 * Clear the cached passphrase.  If CACHEID is not NULL, it will be
 * used instead of a cache ID derived from KEYID.
 */
void
passphrase_clear_cache ( u32 *keyid, const char *cacheid, int algo )
{
#ifdef ENABLE_AGENT_SUPPORT
  assuan_context_t ctx = NULL;
  PKT_public_key *pk;
  byte fpr[MAX_FINGERPRINT_LEN];
  
#if MAX_FINGERPRINT_LEN < 20
#error agent needs a 20 byte fingerprint
#endif
    
  if (!opt.use_agent)
    return;
  
  if (!cacheid)
    {
      pk = xcalloc (1, sizeof *pk);
      memset (fpr, 0, MAX_FINGERPRINT_LEN );
      if( !keyid || get_pubkey( pk, keyid ) )
        {
          goto failure; /* oops: no key for some reason */
        }
  
      {
        size_t dummy;
        fingerprint_from_pk( pk, fpr, &dummy );
      }
    }
  else
    pk = NULL;
    
  if ( !(ctx = agent_open ()) ) 
    goto failure;

  { 
      char *line, *p;
      int i, rc; 

      if (cacheid)
        {
          line = xmalloc (17 + 3*strlen (cacheid) + 2);
          strcpy (line, "CLEAR_PASSPHRASE ");
          p = line+17;
          p = percent_plus_escape (p, cacheid);
        }
      else
        {
          line = xmalloc (17 + 40 + 2);
          strcpy (line, "CLEAR_PASSPHRASE ");
          p = line+17;
          for (i=0; i < 20; i++, p +=2 )
            sprintf (p, "%02X", fpr[i]);
        }
      *p = 0;

      rc = assuan_transact (ctx, line, NULL, NULL, NULL, NULL, NULL, NULL);
      xfree (line);
      if (rc)
        {
          log_error (_("problem with the agent - disabling agent use\n"));
          opt.use_agent = 0;
        }
    }
        
 failure:
  agent_close (ctx);
  if (pk)
    free_public_key( pk );
#endif /*ENABLE_AGENT_SUPPORT*/
}


/****************
 * Ask for a passphrase and return that string.
 */
char *
ask_passphrase (const char *description,
                const char *tryagain_text,
                const char *promptid,
                const char *prompt,
                const char *cacheid, int *canceled)
{
  char *pw = NULL;
  
  if (canceled)
    *canceled = 0;

  if (!opt.batch && description)
    tty_printf ("\n%s\n",description);
               
 agent_died:
  if ( opt.use_agent ) 
    {
      pw = agent_get_passphrase (NULL, 0, cacheid,
                                 tryagain_text, description, prompt,
                                 canceled );
      if (!pw)
        {
          if (!opt.use_agent)
            goto agent_died;
          pw = NULL;
        }
    }
  else if (fd_passwd) 
    {
      pw = m_alloc_secure (strlen(fd_passwd)+1);
      strcpy (pw, fd_passwd);
    }
  else if (opt.batch)
    {
      log_error(_("can't query passphrase in batch mode\n"));
      pw = NULL;
    }
  else {
    if (tryagain_text)
      tty_printf(_("%s.\n"), tryagain_text);
    pw = cpr_get_hidden(promptid? promptid : "passphrase.ask",
                        prompt?prompt : _("Enter passphrase: ") );
    tty_kill_prompt();
  }

  if (!pw || !*pw)
    write_status( STATUS_MISSING_PASSPHRASE );

  return pw;
}


/* Return a new DEK object Using the string-to-key sepcifier S2K.  Use
 * KEYID and PUBKEY_ALGO to prompt the user.

   MODE 0:  Allow cached passphrase
        1:  Ignore cached passphrase 
        2:  Ditto, but change the text to "repeat entry"
*/
DEK *
passphrase_to_dek( u32 *keyid, int pubkey_algo,
		   int cipher_algo, STRING2KEY *s2k, int mode,
                   const char *tryagain_text, int *canceled)
{
    char *pw = NULL;
    DEK *dek;
    STRING2KEY help_s2k;

    if (canceled)
      *canceled = 0;

    if( !s2k ) {
        /* This is used for the old rfc1991 mode 
         * Note: This must match the code in encode.c with opt.rfc1991 set */
	s2k = &help_s2k;
	s2k->mode = 0;
	s2k->hash_algo = S2K_DIGEST_ALGO;
    }

    /* If we do not have a passphrase available in NEXT_PW and status
       information are request, we print them now. */
    if( !next_pw && is_status_enabled() ) {
	char buf[50];
 
	if( keyid ) {
            u32 used_kid[2];
            char *us;

	    if( keyid[2] && keyid[3] ) {
                used_kid[0] = keyid[2];
                used_kid[1] = keyid[3];
            }
            else {
                used_kid[0] = keyid[0];
                used_kid[1] = keyid[1];
            }

            us = get_long_user_id_string( keyid );
            write_status_text( STATUS_USERID_HINT, us );
            m_free(us);

	    sprintf( buf, "%08lX%08lX %08lX%08lX %d 0",
                     (ulong)keyid[0], (ulong)keyid[1],
                     (ulong)used_kid[0], (ulong)used_kid[1],
                     pubkey_algo );
                     
	    write_status_text( STATUS_NEED_PASSPHRASE, buf );
	}
	else {
	    sprintf( buf, "%d %d %d", cipher_algo, s2k->mode, s2k->hash_algo );
	    write_status_text( STATUS_NEED_PASSPHRASE_SYM, buf );
	}
    }

    /* If we do have a keyID, we do not have a passphrase available in
       NEXT_PW, we are not running in batch mode and we do not want to
       ignore the passphrase cache (mode!=1), print a prompt with
       information on that key. */
    if( keyid && !opt.batch && !next_pw && mode!=1 ) {
	PKT_public_key *pk = m_alloc_clear( sizeof *pk );
	char *p;

	p=get_user_id_native(keyid);
	tty_printf("\n");
	tty_printf(_("You need a passphrase to unlock the secret key for\n"
		     "user: \"%s\"\n"),p);
	m_free(p);

	if( !get_pubkey( pk, keyid ) ) {
	    const char *s = pubkey_algo_to_string( pk->pubkey_algo );
	    tty_printf( _("%u-bit %s key, ID %s, created %s"),
		       nbits_from_pk( pk ), s?s:"?", keystr(keyid),
		       strtimestamp(pk->timestamp) );
	    if( keyid[2] && keyid[3] && keyid[0] != keyid[2]
				     && keyid[1] != keyid[3] )
	      {
		if(keystrlen()>10)
		  {
		    tty_printf("\n");
		    tty_printf(_("         (subkey on main key ID %s)"),
			       keystr(&keyid[2]) );
		  }
		else
		  tty_printf( _(" (main key ID %s)"), keystr(&keyid[2]) );
	      }
	    tty_printf("\n");
	}

	tty_printf("\n");
        if (pk)
          free_public_key( pk );
    }

 agent_died:
    if( next_pw ) {
        /* Simply return the passphrase we already have in NEXT_PW. */
	pw = next_pw;
	next_pw = NULL;
    }
    else if ( opt.use_agent ) {
      /* Divert to the gpg-agent. */
        pw = agent_get_passphrase ( keyid, mode == 2? 1: 0, NULL,
                                    tryagain_text, NULL, NULL, canceled );
        if (!pw)
          {
            if (!opt.use_agent)
              goto agent_died;
            pw = m_strdup ("");
          }
        if( *pw && mode == 2 ) {
            char *pw2 = agent_get_passphrase ( keyid, 2, NULL, NULL, NULL,
                                               NULL, canceled );
            if (!pw2)
              {
                if (!opt.use_agent)
                  {
                    m_free (pw);
                    pw = NULL;
                    goto agent_died;
                  }
                pw2 = m_strdup ("");
              }
	    if( strcmp(pw, pw2) ) {
		m_free(pw2);
		m_free(pw);
		return NULL;
	    }
	    m_free(pw2);
	}
    }
    else if( fd_passwd ) {
        /* Return the passphrase we have store in FD_PASSWD. */
	pw = m_alloc_secure( strlen(fd_passwd)+1 );
	strcpy( pw, fd_passwd );
    }
    else if( opt.batch )
      {
	log_error(_("can't query passphrase in batch mode\n"));
	pw = m_strdup( "" ); /* return an empty passphrase */
      }
    else {
        /* Read the passphrase from the tty or the command-fd. */
	pw = cpr_get_hidden("passphrase.enter", _("Enter passphrase: ") );
	tty_kill_prompt();
	if( mode == 2 && !cpr_enabled() ) {
	    char *pw2 = cpr_get_hidden("passphrase.repeat",
				       _("Repeat passphrase: ") );
	    tty_kill_prompt();
	    if( strcmp(pw, pw2) ) {
		m_free(pw2);
		m_free(pw);
		return NULL;
	    }
	    m_free(pw2);
	}
    }

    if( !pw || !*pw )
	write_status( STATUS_MISSING_PASSPHRASE );

    /* Hash the passphrase and store it in a newly allocated DEK
       object.  Keep a copy of the passphrase in LAST_PW for use by
       get_last_passphrase(). */
    dek = m_alloc_secure_clear ( sizeof *dek );
    dek->algo = cipher_algo;
    if( !*pw && mode == 2 )
	dek->keylen = 0;
    else
	hash_passphrase( dek, pw, s2k, mode==2 );
    m_free(last_pw);
    last_pw = pw;
    return dek;
}


/****************
 * Hash a passphrase using the supplied s2k. If create is true, create
 * a new salt or what else must be filled into the s2k for a new key.
 * always needs: dek->algo, s2k->mode, s2k->hash_algo.
 */
static void
hash_passphrase( DEK *dek, char *pw, STRING2KEY *s2k, int create )
{
    MD_HANDLE md;
    int pass, i;
    int used = 0;
    int pwlen = strlen(pw);

    assert( s2k->hash_algo );
    dek->keylen = cipher_get_keylen( dek->algo ) / 8;
    if( !(dek->keylen > 0 && dek->keylen <= DIM(dek->key)) )
	BUG();

    md = md_open( s2k->hash_algo, 1);
    for(pass=0; used < dek->keylen ; pass++ ) {
	if( pass ) {
            md_reset(md);
	    for(i=0; i < pass; i++ ) /* preset the hash context */
		md_putc(md, 0 );
	}

	if( s2k->mode == 1 || s2k->mode == 3 ) {
	    int len2 = pwlen + 8;
	    ulong count = len2;

	    if( create && !pass ) {
		randomize_buffer(s2k->salt, 8, 1);
		if( s2k->mode == 3 )
		    s2k->count = 96; /* 65536 iterations */
	    }

	    if( s2k->mode == 3 ) {
		count = (16ul + (s2k->count & 15)) << ((s2k->count >> 4) + 6);
		if( count < len2 )
		    count = len2;
	    }
	    /* a little bit complicated because we need a ulong for count */
	    while( count > len2 ) { /* maybe iterated+salted */
		md_write( md, s2k->salt, 8 );
		md_write( md, pw, pwlen );
		count -= len2;
	    }
	    if( count < 8 )
		md_write( md, s2k->salt, count );
	    else {
		md_write( md, s2k->salt, 8 );
		count -= 8;
                md_write( md, pw, count );
	    }
	}
	else
	    md_write( md, pw, pwlen );
	md_final( md );
	i = md_digest_length( s2k->hash_algo );
	if( i > dek->keylen - used )
	    i = dek->keylen - used;
	memcpy( dek->key+used, md_read(md, s2k->hash_algo), i );
	used += i;
    }
    md_close(md);
}

