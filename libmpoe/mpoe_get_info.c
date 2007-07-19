#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "mpoe_io.h"
#include "mpoe_lib.h"
#include "mpoe_internals.h"

/* returns the max amount of boards */
mpoe_return_t
mpoe__get_board_max(uint32_t * max)
{
  mpoe_return_t ret = MPOE_SUCCESS;
  int err, fd;

  err = open(MPOE_DEVNAME, O_RDONLY);
  if (err < 0) {
    ret = mpoe__errno_to_return(errno, "open");
    goto out;
  }
  fd = err;

  err = ioctl(fd, MPOE_CMD_GET_BOARD_MAX, max);
  if (err < 0) {
    ret = mpoe__errno_to_return(errno, "ioctl GET_BOARD_MAX");
    goto out_with_fd;
  }

 out_with_fd:
  close(fd);
 out:
  return ret;
}

/* returns the current amount of boards */
mpoe_return_t
mpoe__get_board_count(uint32_t * count)
{
  mpoe_return_t ret = MPOE_SUCCESS;
  int err, fd;

  err = open(MPOE_DEVNAME, O_RDONLY);
  if (err < 0) {
    ret = mpoe__errno_to_return(errno, "open");
    goto out;
  }
  fd = err;

  err = ioctl(fd, MPOE_CMD_GET_BOARD_COUNT, count);
  if (err < 0) {
    ret = mpoe__errno_to_return(errno, "ioctl GET_BOARD_COUNT");
    goto out_with_fd;
  }

 out_with_fd:
  close(fd);
 out:
  return ret;
}

/* returns the board id of the endpoint, or the current one of the index */
mpoe_return_t
mpoe__get_board_id(struct mpoe_endpoint * ep, uint8_t * index,
		   char * name, struct mpoe_mac_addr * addr)
{
  mpoe_return_t ret = MPOE_SUCCESS;
  struct mpoe_cmd_get_board_id board_id;
  int err, fd;

  if (ep) {
    /* use the endpoint */
    fd = ep->fd;
  } else {
    /* use a dummy endpoint and the index */
    err = open(MPOE_DEVNAME, O_RDONLY);
    if (err < 0) {
      ret = mpoe__errno_to_return(errno, "open");
      goto out;
    }
    fd = err;

    board_id.board_index = *index;
  }

  err = ioctl(fd, MPOE_CMD_GET_BOARD_ID, &board_id);
  if (err < 0) {
    ret = mpoe__errno_to_return(errno, "ioctl GET_BOARD_ID");
    goto out_with_fd;
  }

  strncpy(name, board_id.board_name, MPOE_IF_NAMESIZE);
  *index = board_id.board_index;
  mpoe_mac_addr_copy(addr, &board_id.board_addr);

 out_with_fd:
  if (!ep)
    close(fd);
 out:
  return ret;
}

/* returns the current index of a board given by its name */
mpoe_return_t
mpoe__get_board_index_by_name(const char * name, uint8_t * index)
{
  mpoe_return_t ret = MPOE_SUCCESS;
  uint32_t max;
  int err, fd, i;

  err = open(MPOE_DEVNAME, O_RDONLY);
  if (err < 0) {
    ret = mpoe__errno_to_return(errno, "open");
    goto out;
  }
  fd = err;

  err = ioctl(fd, MPOE_CMD_GET_BOARD_MAX, &max);
  if (err < 0) {
    ret = mpoe__errno_to_return(errno, "ioctl GET_BOARD_MAX");
    goto out_with_fd;
  }

  ret = MPOE_INVALID_PARAMETER;
  for(i=0; i<max; i++) {
    struct mpoe_cmd_get_board_id board_id;

    board_id.board_index = i;
    err = ioctl(fd, MPOE_CMD_GET_BOARD_ID, &board_id);
    if (err < 0) {
      ret = mpoe__errno_to_return(errno, "ioctl GET_BOARD_ID");
      if (ret != MPOE_INVALID_PARAMETER)
	goto out_with_fd;
    }

    if (!strncmp(name, board_id.board_name, MPOE_IF_NAMESIZE)) {
      ret = MPOE_SUCCESS;
      *index = i;
      break;
    }
  }

 out_with_fd:
  close(fd);
 out:
  return ret;
}
