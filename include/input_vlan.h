/*****************************************************************************
 * input_vlan.h: vlan input method
 * (c)1999 VideoLAN
 *****************************************************************************
 * ??
 *****************************************************************************
 * Required headers:
 * <netinet/in.h>
 * "vlc_thread.h"
 *****************************************************************************/

/*****************************************************************************
 * Prototypes
 *****************************************************************************/
int     input_VlanCreate        ( void );
void    input_VlanDestroy       ( void );
int     input_VlanJoin          ( int i_vlan_id );
void    input_VlanLeave         ( int i_vlan_id );



